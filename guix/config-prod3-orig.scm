;; prod3 (x99gpu2) Guix config — headless NVIDIA inference node.
;; PINNED: guix df85457 + nonguix c15e19c. Classic nonguix API.
;; Adds: bare microcode-initrd + declarative persistence (nvidia-pm-set).

(use-modules (gnu)
             (gnu services shepherd)
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
  (firmware (cons* linux-firmware %base-firmware))
  (kernel-arguments (cons* "modprobe.blacklist=nouveau"
                           %default-kernel-arguments))

  ;; No (kernel-loadable-modules ...): nvidia-service-type wires the module.
  (services
   (append (list (service openssh-service-type)
                 (service network-manager-service-type)
                 (service wpa-supplicant-service-type)
                 (service ntp-service-type)
                 (service nvidia-service-type)
                 (simple-service 'nvidia-pm-set
                                 shepherd-root-service-type
                                 (list (shepherd-service
                                        (provision '(nvidia-pm-set))
                                        (requirement '(user-processes))
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