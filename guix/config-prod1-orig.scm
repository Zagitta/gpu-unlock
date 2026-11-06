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
  (firmware (cons* linux-firmware %base-firmware))
  (kernel-arguments (cons* "modprobe.blacklist=nouveau"
                           %default-kernel-arguments))
  (services (append
             (list
              (service openssh-service-type)
              (service network-manager-service-type)
              (service wpa-supplicant-service-type)
              (service ntp-service-type)
              (service nvidia-service-type)
              (simple-service 'nvidia-pm-set
                              shepherd-root-service-type
                              (list (shepherd-service
                                     (provision '(nvidia-pm-set))
                                     (requirement '(user-processes))
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
                                     (requirement '(user-processes nvidia-pm-set))
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
                                     (requirement '(user-processes nvidia-pm-set))
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