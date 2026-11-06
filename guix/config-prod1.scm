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
  (host-name "prod1")
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
  (services (append
             (list
              (service openssh-service-type)
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
                                     (documentation "persistence")
                                     (one-shot? #t)
                                     (auto-start? #t)
                                     (respawn? #f)
                                     (start #~(make-forkexec-constructor
                                               (list "/bin/sh" "-c"
                                                     (string-append
                                                      #$(file-append nvidia-driver "/bin/nvidia-smi")
                                                      " -pm 1 && "
                                                      #$(file-append nvidia-driver "/bin/nvidia-smi")
                                                      " -pl 260")))))))
              (simple-service 'llama-server-8099
                              shepherd-root-service-type
                              (list (shepherd-service
                                     (provision '(llama-server-8099))
                                     (requirement '(user-processes nvidia-pm-set gpu-unlock-load))
                                     (documentation "Qwen3.8-27B-Uncensored llama.cpp server (GPU0, :8099)")
                                     (respawn? #t)
                                     (start #~(make-forkexec-constructor
                                               (list "/home/r/llama-cuda/llama-server"
                                                     "-m" "/home/r/models/Qwen3.8-27B-Uncensored-IQ4_XS.gguf"
                                                     "--alias" "qwen38"
                                                     "-ngl" "99"
                                                     "-c" "245000"
                                                     "--cache-type-k" "q8_0"
                                                     "--cache-type-v" "q8_0"
                                                     "--host" "0.0.0.0"
                                                     "--port" "8099")
                                               #:user "r"
                                               #:group "users"
                                               #:environment-variables
                                               (list (string-append
                                                      "LD_LIBRARY_PATH=/home/r/llama-cuda:/home/r/cuda/lib64:/home/r/cuda/lib:"
                                                      #$(file-append
                                                         nvidia-driver "/lib:") )
                                                     "CUDA_PATH=/home/r/cuda"
                                                     "CUDA_HOME=/home/r/cuda"
                                                     "CUDA_VISIBLE_DEVICES=0")
                                               #:log-file "/home/r/llama-server-8099.log"))
                                     (stop #~(make-kill-destructor)))))
              (simple-service 'llama-server-8100
                              shepherd-root-service-type
                              (list (shepherd-service
                                     (provision '(llama-server-8100))
                                     (requirement '(user-processes nvidia-pm-set gpu-unlock-load))
                                     (documentation "Qwen3.8-27B-Uncensored llama.cpp server (GPU1, :8100)")
                                     (respawn? #t)
                                     (start #~(make-forkexec-constructor
                                               (list "/home/r/llama-cuda/llama-server"
                                                     "-m" "/home/r/models/Qwen3.8-27B-Uncensored-IQ4_XS.gguf"
                                                     "--alias" "qwen38"
                                                     "-ngl" "99"
                                                     "-c" "245000"
                                                     "--cache-type-k" "q8_0"
                                                     "--cache-type-v" "q8_0"
                                                     "--host" "0.0.0.0"
                                                     "--port" "8100")
                                               #:user "r"
                                               #:group "users"
                                               #:environment-variables
                                               (list (string-append
                                                      "LD_LIBRARY_PATH=/home/r/llama-cuda:/home/r/cuda/lib64:/home/r/cuda/lib:"
                                                      #$(file-append
                                                         nvidia-driver "/lib:") )
                                                     "CUDA_PATH=/home/r/cuda"
                                                     "CUDA_HOME=/home/r/cuda"
                                                     "CUDA_VISIBLE_DEVICES=1")
                                               #:log-file "/home/r/llama-server-8100.log"))
                                     (stop #~(make-kill-destructor))))))
             %base-services))
  (bootloader (bootloader-configuration
                (bootloader grub-efi-bootloader)
                (targets (list "/boot/efi"))
                (keyboard-layout keyboard-layout)))
  (swap-devices (list (swap-space
                        (target (uuid
                                 "4cc44e46-a865-42c5-86e5-50b21f7d0f50")))))
  (file-systems (cons* (file-system
                         (mount-point "/boot/efi")
                         (device (uuid "8B42-3231" 'fat32))
                         (type "vfat"))
                       (file-system
                         (mount-point "/")
                         (device (uuid "39948402-d2c4-4f10-94e6-0fa98f06b875"
                                       'ext4))
                         (type "ext4")) %base-file-systems)))