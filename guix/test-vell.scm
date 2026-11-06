(use-modules (gpu-unlock)
             (gnu packages linux))

;; Compile-only test: builds the module against the current linux-libre.
(list (make-gpu-unlock-module linux-libre)
      gpu-unlock-firmware)
