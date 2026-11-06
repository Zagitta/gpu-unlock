;;; gpu-unlock.scm --- Guix packaging for Zagitta/gpu-unlock.

;;; SPDX-License-Identifier: GPL-2.0-or-later

;;; Commentary:
;;
;; Packages for running gpu_unlock on Guix System:
;;
;;   (make-gpu-unlock-module <kernel-package>) -- out-of-tree kernel module
;;   gpu-unlock-firmware                       -- firmware blobs it loads
;;
;; The module is INERT unless the kernel command line contains
;; `gpu_unlock.targets=all`.  It must run BEFORE the nvidia driver binds
;; the GPU; see guix/integration.md in this repository for the
;; blacklist + ordered-modprobe shepherd recipe.
;;
;; Load the file from a system config with:
;;   guix system build/reconfigure -L /path/to/repo/guix config.scm

;;; Code:

(define-module (gpu-unlock)
  #:use-module (guix packages)
  #:use-module (guix download)
  #:use-module (guix git-download)
  #:use-module (guix gexp)
  #:use-module (guix build-system copy)
  #:use-module (guix build-system linux-module)
  #:use-module ((guix licenses) #:prefix license:)
  #:use-module (gnu packages base)
  #:use-module (gnu packages bash)
  #:use-module (gnu packages compression)
  #:use-module (gnu packages python)
  #:export (make-gpu-unlock-module
            gpu-unlock-firmware
            gpu-unlock-source
            %gpu-unlock-version))

(define %gpu-unlock-version "2.0.0")
(define %gpu-unlock-commit "7eb51bccae0a0b7e599e084d971170e9a1e82d0c")

(define gpu-unlock-source
  (origin
    (method git-fetch)
    (uri (git-reference
          (url "https://github.com/Zagitta/gpu-unlock")
          (commit %gpu-unlock-commit)))
    (file-name (git-file-name "gpu-unlock" %gpu-unlock-version))
    (sha256
     (base32
      "1m84a37xk2qha65pj8g6zvds6yqm11x0vrv3prb2b46dzbhg62bq"))))

(define nvidia-kernel-source-610.43.03
  (origin
    (method url-fetch)
    (uri (string-append
          "https://download.nvidia.com/XFree86/NVIDIA-kernel-module-source/"
          "NVIDIA-kernel-module-source-610.43.03.tar.xz"))
    (file-name "NVIDIA-kernel-module-source-610.43.03.tar.xz")
    (sha256
     (base32
      "1p61kzz2vb770cia95c6z6d4xq738qx2fqsd24vdqgm2qwiqj4by"))))

(define nvidia-installer-610.43.03
  (origin
    (method url-fetch)
    (uri (string-append
          "https://download.nvidia.com/XFree86/Linux-x86_64/610.43.03/"
          "NVIDIA-Linux-x86_64-610.43.03-no-compat32.run"))
    (file-name "NVIDIA-Linux-x86_64-610.43.03-no-compat32.run")
    (sha256
     (base32
      "141gzqc47s05a95xkxs5r7ikdln495bnakypsclxz9ihnwflb170"))))

(define %fw-dir "lib/firmware/gpu-unlock/610.43.03")

(define gpu-unlock-firmware
  (package
    (name "gpu-unlock-firmware")
    (version %gpu-unlock-version)
    (source gpu-unlock-source)
    (build-system copy-build-system)
    (arguments
     (list
      #:install-plan #~'()
      #:phases
      #~(modify-phases %standard-phases
          (add-after 'unpack 'extract-nvidia-artifacts
            (lambda* (#:key inputs #:allow-other-keys)
              ;; extract_gsp.py <installer.run> <outdir>  (reads the
              ;; .run archive directly; needs `zstd` in PATH)
              ;; extract_boot_assets.py <kernel-source-dir> <outdir>
              ;; Both write the *.bin assets FLAT into <outdir>.
              (mkdir-p "assets")
              (invoke "tar" "-xf" (assoc-ref inputs "nvidia-source")
                      "-C" ".")
              (invoke "python3" "firmware/extract_gsp.py"
                      (assoc-ref inputs "nvidia-installer")
                      "assets")
              (invoke "python3" "firmware/extract_boot_assets.py"
                      "NVIDIA-kernel-module-source-610.43.03"
                      "assets")
              (delete-file-recursively
               "NVIDIA-kernel-module-source-610.43.03")
              (let ((target (string-append #$output
                             "/lib/firmware/gpu-unlock/610.43.03")))
                (mkdir-p target)
                (for-each
                 (lambda (f)
                   (copy-file f (string-append target "/" (basename f))))
                 (find-files "assets" "\\.bin$"))
                (format #t "installed ~a firmware files to ~a~%"
                        (length (find-files target)) target))
              #t)))))
    (native-inputs (list python tar xz zstd))
    (inputs `(("nvidia-source" ,nvidia-kernel-source-610.43.03)
              ("nvidia-installer" ,nvidia-installer-610.43.03)))
    (home-page "https://github.com/Zagitta/gpu-unlock")
    (synopsis "Firmware assets for the gpu_unlock kernel module")
    (description
     "GSP and booter assets extracted from NVIDIA 610.43.03 that the
gpu_unlock out-of-tree kernel module requests from
@file{@%fw-dir}.  Built by the repository's extract scripts.")
    (license (list license:gpl2 license:cc0)))) ; see firmware/NOTICE.md

(define* (make-gpu-unlock-module linux)
  "Return the gpu_unlock out-of-tree kernel module package built against
LINUX (use the exact kernel package the target system config uses)."
  (package
    (name "gpu-unlock-module")
    (version %gpu-unlock-version)
    (source gpu-unlock-source)
    (build-system linux-module-build-system)
    (arguments
     (list
      #:linux linux
      #:source-directory "kernel/gpu_unlock"
      #:tests? #f))
    (home-page "https://github.com/Zagitta/gpu-unlock")
    (synopsis "Unlock full-rate tensor cores on GeForce RTX 20/30/40")
    (description
     "Out-of-tree kernel module that lifts NVIDIA's market-segmentation
cap which runs GeForce tensor cores at half rate for FP16/BF16 and
TF32-with-FP32-accumulate math.  It writes the speed-select registers
and performs a Function-Level Reset before the NVIDIA driver binds the
device; outputs are bit-identical.  Inert unless the kernel command
line contains @code{gpu_unlock.targets=all}.")
    (license license:gpl2)))

;;; gpu-unlock.scm ends here
