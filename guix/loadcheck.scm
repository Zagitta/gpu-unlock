(with-input-from-file "/home/r/src/gpu-unlock/guix/config-prod3.scm"
  (lambda ()
    (let lp ()
      (let ((x (read)))
        (unless (eof-object? x) (lp))))))
(display "READ-OK\n")
