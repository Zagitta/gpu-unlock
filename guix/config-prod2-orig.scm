;; prod2 (x99gpu3) Guix config — headless NVIDIA inference node.
;; PINNED: guix df85457 + nonguix c15e19c. Classic nonguix API.
;; Adds: bare microcode-initrd + declarative persistence (nvidia-pm-set)
;;       + two 27B llama-server services, one per 3090.
;;
;; Pool layout (verified 2026-08-30):
;;   llama-server-gpu0: IQ4_XS, -c 245000 (VRAM ceiling; 250K OOMs),
;;                      q8_0 KV, port 8099
;;   llama-server-gpu1: same, port 8100
;;   Caddy (bsgw2) /v1/prod2/* round-robins 63:8099 + 63:8100.
;;   Each server runs 4 auto slots (shared kv-unified pool) and queues
;;   internally, so p2 = 8 concurrent 27B slots behind one endpoint.
;;   MTP spec does NOT fit at 245K (needs ~264MB); add
;;   --spec-type draft-mtp --spec-draft-n-max 1 only at <=160K ctx.

(use-modules (gnu)
             (gnu services shepherd)
             (nongnu packages linux)
             (nongnu packages nvidia)
             (nongnu services nvidia)
             (nongnu system linux-initrd))
(use-service-modules networking ssh)

(define llama-server-args
  (lambda (port gpu)
    (shepherd-service
        (provision (list (string->symbol (string-append "llama-server-" port))))
        (requirement '(user-processes nvidia-pm-set))
        (documentation (string-append "Qwen3.8-27B-Uncensored IQ4_XS llama.cpp server, GPU " gpu))
        (respawn? #t)
        (start #~(make-forkexec-constructor
                   (list "/home/r/llama-cuda/llama-server"
                         "-m" "/home/r/models/qwen38-27b-uncensored/Qwen3.8-27B-Uncensored-IQ4_XS.gguf"
                         "--alias" "qwen38"
                         "-ngl" "99"
                         "-c" "245000"
                         "--cache-type-k" "q8_0"
                         "--cache-type-v" "q8_0"
                         "--host" "0.0.0.0"
                         "--port" #$port)
                   #:user "r"
                   #:group "users"
                   #:environment-variables
                   (list (string-append
                          "LD_LIBRARY_PATH=/home/r/llama-cuda:/home/r/cuda/lib64:/home/r/cuda/lib:/"
                          #$(file-append nvidia-driver "/lib"))
                         "CUDA_PATH=/home/r/cuda"
                         "CUDA_HOME=/home/r/cuda"
                         (string-append "CUDA_VISIBLE_DEVICES=" #$gpu))
                   #:log-file (string-append "/home/r/llama-server-" #$port ".log")))
        (stop #~(make-kill-destructor)))))

(operating-system
  (locale "en_US.utf8")
  (timezone "America/New_York")
  (keyboard-layout (keyboard-layout "us"))
  (host-name "prod2")

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
                                                  (list "/bin/sh" "-c"
                                                       (string-append
                                                        #$(file-append nvidia-driver "/bin/nvidia-smi")
                                                        " -pm 1 && "
                                                        #$(file-append nvidia-driver "/bin/nvidia-smi")
                                                        " -pl 260")))))))
                 (simple-service 'llama-server-gpu0
                                 shepherd-root-service-type
                                 (list (llama-server-args "8099" "0")))
                 (simple-service 'llama-server-gpu1
                                 shepherd-root-service-type
                                 (list (llama-server-args "8100" "1"))))
           %base-services))

  (bootloader (bootloader-configuration
                (bootloader grub-efi-bootloader)
                (targets (list "/boot/efi"))
                (keyboard-layout keyboard-layout)))
  (swap-devices (list (swap-space
                        (target (uuid
                                 "77dd8d02-6c3b-48d9-a48d-a15a1dd0a7c0")))))

  (file-systems (cons* (file-system
                         (mount-point "/boot/efi")
                         (device (uuid "44F6-D35C"
                                       'fat32))
                         (type "vfat"))
                       (file-system
                         (mount-point "/")
                         (device (uuid
                                  "daee4d7f-9efc-412e-9b44-19779c0895d9"
                                  'ext4))
                         (type "ext4")) %base-file-systems)))
