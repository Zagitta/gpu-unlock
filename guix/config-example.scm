;; gpu-unlock on Guix System — fragment to add to an existing
;; operating-system declaration (nonguix, classic API).
;;
;; Do NOT use this file verbatim as a full config: keep your own
;; bootloader/users/services and add the marked fields.  The one-shot
;; service is the complete, boot-proven version: it stops nvidia-powerd,
;; freezes the udev exec queue, sweeps /dev/nvidia* holders, ejects the
;; bound driver stack, loads gpu_unlock (which writes the speed
;; selectors and FLRs the cards), resumes udev, then loads the NVIDIA
;; stack explicitly with modprobe -d pointing at the booted system's
;; kernel directory (modprobe in a service context cannot see Guix's
;; modules under /lib/modules).
;;
;; The module package and firmware come from this repo's guix/ module:
;;   (use-modules (gpu-unlock))   ; guix system ... -L <repo>/guix
;;
;; Rollback: remove "gpu_unlock.targets=all" and the nvidia blacklist
;; entries from kernel-arguments, or boot the previous generation —
;; gpu_unlock without the cmdline flag is inert.

(use-modules (gnu)
             (gnu packages linux)        ; kmod
             (nongnu packages linux)     ; nonguix linux kernel
             (nongnu system linux-initrd)
             (nongnu services nvidia)
             (gpu-unlock))               ; via -L <repo>/guix

;; --- add to your operating-system -------------------------------------
(operating-system
  (kernel linux)                                   ; keep your kernel
  (kernel-loadable-modules (list (make-gpu-unlock-module linux)))
  (firmware (cons* gpu-unlock-firmware
                   ;; your other firmware here
                   %base-firmware))
  (kernel-arguments (cons* "modprobe.blacklist=nouveau,nvidia,nvidia_modeset,nvidia_uvm,nvidia_drm"
                           "gpu_unlock.targets=all"
                           %default-kernel-arguments))

  ;; Boot-time unlock one-shot.  Runs after file-systems, before the
  ;; NVIDIA driver binds.  Any shepherd service that opens CUDA (or the
  ;; nvidia power-cap service) must require gpu-unlock-load so it starts
  ;; only after the unlock completes — otherwise it holds the driver and
  ;; the unlock silently no-ops.
                   (simple-service 'gpu-unlock-load
                                 shepherd-root-service-type
                                 (list (shepherd-service
                                        (provision '(gpu-unlock-load))
                                        (requirement '(file-systems))
                                        (one-shot? #t)
                                        (auto-start? #t)
                                        (start #~(lambda _
                                                   (let* ((m (string-append #$kmod
                                                                            "/bin/modprobe"))
                                                          (ud (string-append #$eudev
                                                                             "/bin/udevadm"))
                                                          (pk "/run/current-system/profile/bin/pkill")
                                                          (p "/sys/module/firmware_class/parameters/path")
                                                          (log (open-output-file
                                                                "/run/gpu-unlock-load.log"))
                                                          (say (lambda (s)
                                                                 (display s log)
                                                                 (force-output log)))
                                                          (empty? (lambda _
                                                                    (catch #t
                                                                      (lambda ()
                                                                        (let ((s (call-with-input-file
                                                                                  p
                                                                                  read-line)))
                                                                          (or (not (string? s))
                                                                              (string=? s ""))))
                                                                      (lambda _ #t))))
                                                          (sh (lambda args
                                                                (zero?
                                                                 (system* "/bin/sh" "-c"
                                                                          (string-append
                                                                           m " -d /run/booted-system/kernel "
                                                                           (string-join (cdr args) " ")
                                                                           " 2>> /run/gpu-unlock-load.log"))))))
                                                     (chmod log #o644)
                                                     ;; Guix exposes kernel modules only
                                                     ;; via this env var - without it
                                                     ;; modprobe reports "not found in
                                                     ;; /lib/modules/$(uname -r)".
                                                     (setenv "LINUX_MODULE_DIRECTORY"
                                                             "/run/booted-system/kernel/lib/modules")
                                                     (say "start\n")
                                                     ;; Wait for activation to set the
                                                     ;; kernel firmware path (max 60s).
                                                     (let loop ((n 60))
                                                       (when (and (empty?) (> n 0))
                                                         (sleep 1)
                                                         (loop (- n 1))))
                                                     ;; Stop powerd VIA SHEPHERD (pkill
                                                     ;; alone triggers instant respawn;
                                                     ;; powerd holds the driver open).
                                                     (system* "/run/current-system/profile/bin/herd"
                                                              "stop" "nvidia-powerd")
                                                     (system* pk "-x" "nvidia-powerd")
                                                     (say "powerd stopped\n")
                                                     (system* ud "control"
                                                              "--stop-exec-queue")
                                                     (say "udev queue stopped\n")
                                                    ;; Kill anything holding /dev/nvidia*
                                                    ;; open (boot-managed GPU daemons like
                                                    ;; omni grab the device before this
                                                    ;; service runs) - the FLR needs a free
                                                    ;; device and held uvm blocks the eject.
                                                    ;; Kill anything holding /dev/nvidia*
                                                    ;; open (boot-managed GPU daemons like
                                                    ;; omni grab the device before this
                                                    ;; service runs) - the FLR needs a free
                                                    ;; device and held uvm blocks the eject.
                                                    ;; Pure-shell scan (scandir/filter-map
                                                    ;; are unbound in this compiled module).
                                                    (let ((shk (lambda (c)
                                                                 (system* "/bin/sh" "-c"
                                                                          (string-append
                                                                           c " >> /run/gpu-unlock-load.log 2>&1")))))
                                                      (shk "for p in /proc/[0-9]*; do if ls -l $p/fd 2>/dev/null | grep -aq /dev/nvidia; then echo holder: ${p#/proc/}; kill -TERM ${p#/proc/} 2>/dev/null; fi; done")
                                                      (sleep 1)
                                                      (shk "for p in /proc/[0-9]*; do if ls -l $p/fd 2>/dev/null | grep -aq /dev/nvidia; then kill -KILL ${p#/proc/} 2>/dev/null; fi; done")
                                                      (sleep 1)
                                                      (shk "for p in /proc/[0-9]*; do if ls -l $p/fd 2>/dev/null | grep -aq /dev/nvidia; then echo still-alive: ${p#/proc/}; fi; done"))
                                                    (say "holders cleared\n")
                                                     (let eject ((mods '("nvidia_uvm"
                                                                         "nvidia_drm"
                                                                         "nvidia_modeset"
                                                                         "nvidia"))
                                                                 (tries 8))
                                                       (cond ((null? mods) #t)
                                                             ((sh m "-r" (car mods))
                                                              (eject (cdr mods) 8))
                                                             ((> tries 1)
                                                              (sleep 1)
                                                              (eject mods (- tries 1)))
                                                             (else (say "eject failed\n")
                                                                   #f)))
                                                     (say "stack unloaded\n")
                                                     ;; Apply the unlock; on failure the
                                                     ;; racer may have rebound - eject
                                                     ;; and retry.
                                                     (let apply ((tries 3))
                                                       (cond ((sh m "gpu_unlock")
                                                              (say "gpu_unlock loaded\n"))
                                                             ((> tries 1)
                                                              (for-each
                                                               (lambda (mod) (sh m "-r" mod))
                                                               '("nvidia_uvm" "nvidia_drm"
                                                                 "nvidia_modeset" "nvidia"))
                                                              (sleep 1)
                                                              (apply (- tries 1)))
                                                             (else (say "gpu_unlock FAILED\n"))))
                                                     ;; Resume udev events - any queued
                                                     ;; nvidia-modprobe now just binds the
                                                     ;; driver on already-unlocked silicon.
                                                     (system* ud "control" "--start-exec-queue")
                                                     (say "udev queue resumed\n")
                                                     ;; Bring the stack back up unlocked.
                                                     (if (sh m "nvidia_drm")
                                                         (if (sh m "nvidia_uvm")
                                                             (say "nvidia loaded unlocked\n")
                                                             (say "nvidia_uvm FAILED\n"))
                                                         (say "nvidia_drm FAILED\n"))
                                                     #t)))
                                        (stop #~(const #f)))))
  ;; (service nvidia-service-type) stays as before.
  ;; (simple-service 'nvidia-pm-set ...): add gpu-unlock-load to its
  ;; requirement list, e.g. (requirement '(user-processes gpu-unlock-load)).
  )
