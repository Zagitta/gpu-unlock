;; prod3 (x99gpu2) Guix config — headless NVIDIA inference node.
;; PINNED: guix df85457 + nonguix c15e19c. Classic nonguix API.
;; Adds: bare microcode-initrd + declarative persistence (nvidia-pm-set).
;;
;; gpu-unlock port: kernel module + firmware via (gpu-unlock) load path
;; (-L <repo>/guix).  Load order is deterministic: nvidia* is blacklisted
;; on the cmdline (udev coldplug can't claim the GPUs), and the
;; gpu-unlock-load one-shot modprobes gpu_unlock then the NVIDIA stack.
;; Rollback: delete "gpu_unlock.targets=all" and the nvidia blacklist
;; entries from kernel-arguments.

(use-modules (gnu)
             (gnu packages linux)             ; kmod, eudev
             (gnu services shepherd)
             (gpu-unlock)                     ; -L <repo>/guix
             (nongnu packages linux)
             (nongnu packages nvidia)
             (nongnu services nvidia)
             (nongnu system linux-initrd))
(use-service-modules networking ssh)

(operating-system
  (locale "en_US.utf8")
  (timezone "America/New_York")
  (keyboard-layout (keyboard-layout "us"))
  (host-name "prod3")

  (users (cons* (user-account
                  (name "r")
                  (comment "R")
                  (group "users")
                  (home-directory "/home/r")
                  (supplementary-groups '("wheel" "netdev" "audio" "video")))
                %base-user-accounts))

  (kernel linux)
  (initrd microcode-initrd)
  (kernel-loadable-modules (list (make-gpu-unlock-module linux)))
  (firmware (cons* gpu-unlock-firmware
                   linux-firmware
                   %base-firmware))
  (kernel-arguments (cons* "modprobe.blacklist=nouveau,nvidia,nvidia_modeset,nvidia_uvm,nvidia_drm"
                           "gpu_unlock.targets=all"
                           %default-kernel-arguments))

  ;; gpu-unlock-load runs before anything that needs the driver.
  (services
   (append (list (service openssh-service-type)
                 (service network-manager-service-type)
                 (service wpa-supplicant-service-type)
                 (service ntp-service-type)
                 (service nvidia-service-type)
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
                 (simple-service 'nvidia-pm-set
                                 shepherd-root-service-type
                                 (list (shepherd-service
                                        (provision '(nvidia-pm-set))
                                        (requirement '(user-processes gpu-unlock-load))
                                        (one-shot? #t)
                                        (auto-start? #t)
                                        (start #~(make-forkexec-constructor
                                                  (list #$(file-append
                                                           nvidia-driver
                                                           "/bin/nvidia-smi")
                                                        "-pm" "1")))))))
           %base-services))

  (bootloader (bootloader-configuration
                (bootloader grub-efi-bootloader)
                (targets (list "/boot/efi"))
                (keyboard-layout keyboard-layout)))
  (swap-devices (list (swap-space
                        (target (uuid
                                 "a13ae853-55a7-46ff-930b-82941491437a")))))

  (file-systems (cons* (file-system
                         (mount-point "/boot/efi")
                         (device (uuid "6FCE-A81B"
                                       'fat32))
                         (type "vfat"))
                       (file-system
                         (mount-point "/")
                         (device (uuid
                                  "986e32ec-ee77-40ec-beb9-9d7fca31c9c3"
                                  'ext4))
                         (type "ext4")) %base-file-systems)))
