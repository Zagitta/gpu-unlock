;; Channels for prod3 gpu-unlock system build. Pinned to the verified
;; latest-generation commits (2026-11-05): guix 4f5d464, nonguix 2b2e100,
;; kernel 7.2.15 — module/firmware/system already built for these.
;; nonguix introduction required for commit authentication.
(list (channel
        (name 'guix)
        (url "https://git.guix.gnu.org/guix.git")
        (branch "master")
        (commit "4f5d464ceb00a1bd0ead6ecf327f1ea87f5254b0"))
      (channel
        (name 'nonguix)
        (url "https://gitlab.com/nonguix/nonguix")
        (commit "2b2e1002c620ec53115d20c96ecdd2ce7236da25")
        (introduction
         (make-channel-introduction
          "897c1a470da759236cc11798f4e0a5f7d4d59fbc"
          (openpgp-fingerprint
           "2A39 3FFF 68F4 EF7A 3D29  12AF 6F51 20A0 22FB B2D5")))))
