;;; GNU Guix --- Functional package management for GNU
;;; Copyright © 2012-2021, 2025 Ludovic Courtès <ludo@gnu.org>
;;; Copyright © 2013 Nikita Karetnikov <nikita@karetnikov.org>
;;; Copyright © 2013 Andreas Enge <andreas@enge.fr>
;;; Copyright © 2015, 2018 Mark H Weaver <mhw@netris.org>
;;; Copyright © 2018 Arun Isaac <arunisaac@systemreboot.net>
;;; Copyright © 2018, 2019 Ricardo Wurmus <rekado@elephly.net>
;;; Copyright © 2021, 2022 Maxime Devos <maximedevos@telenet.be>
;;; Copyright © 2020 Christine Lemmer-Webber <cwebber@dustycloud.org>
;;; Copyright © 2021 Brice Waegeneire <brice@waegenei.re>
;;; Copyright © 2022 Tobias Geerinckx-Rice <me@tobias.gr>
;;; Copyright © 2024 Nicolas Graves <ngraves@ngraves.fr>
;;; Copyright © 2024 Giacomo Leidi <goodoldpaul@autistici.org>
;;; Copyright © 2025 Maxim Cournoyer <maxim@guixotic.coop>
;;;
;;; This file is part of GNU Guix.
;;;
;;; GNU Guix is free software; you can redistribute it and/or modify it
;;; under the terms of the GNU General Public License as published by
;;; the Free Software Foundation; either version 3 of the License, or (at
;;; your option) any later version.
;;;
;;; GNU Guix is distributed in the hope that it will be useful, but
;;; WITHOUT ANY WARRANTY; without even the implied warranty of
;;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;;; GNU General Public License for more details.
;;;
;;; You should have received a copy of the GNU General Public License
;;; along with GNU Guix.  If not, see <http://www.gnu.org/licenses/>.

(define-module (gnu build activation)
  #:use-module (gnu system accounts)
  #:use-module (gnu system privilege)
  #:use-module (gnu build accounts)
  #:use-module (gnu build linux-boot)
  #:use-module (guix build utils)
  #:use-module ((guix build syscalls) #:select (with-file-lock))
  #:use-module (ice-9 ftw)
  #:use-module (ice-9 match)
  #:use-module (ice-9 vlist)
  #:use-module (srfi srfi-1)
  #:use-module (srfi srfi-11)
  #:use-module (srfi srfi-26)
  #:export (activate-users+groups
            activate-subuids+subgids
            activate-user-home
            activate-etc
            activate-privileged-programs
            activate-special-files
            activate-modprobe
            activate-firmware
            activate-ptrace-attach
            activate-current-system
            mkdir-p/perms))

;;; Commentary:
;;;
;;; This module provides "activation" helpers.  Activation is the process that
;;; consists in setting up system-wide files and directories so that an
;;; 'operating-system' configuration becomes active.
;;;
;;; Code:

(define %skeleton-directory
  ;; Directory containing skeleton files for new accounts.
  ;; Note: keep the trailing '/' so that 'scandir' enters it.
  "/etc/skel/")

(define (dot-or-dot-dot? file)
  (member file '("." "..")))

(define (mkdir-p/perms directory owner bits)
  "Create directory DIRECTORY and all its ancestors.

Additionally, verify no component of DIRECTORY is a symbolic link,
without TOCTTOU races.  However, if OWNER differs from the the current
(process) uid/gid, there is a small window in which DIRECTORY is set to the
current (process) uid/gid instead of OWNER.  This is not expected to be
a problem in practice.

The permission bits and owner of DIRECTORY are set to BITS and OWNER.
Anything above DIRECTORY that already exists keeps
its old owner and bits.  For components that do not exist yet, the owner
and bits are set according to the default behaviour of 'mkdir'."
  (define absolute?
    (string-prefix? "/" directory))

  (define not-slash
    (char-set-complement (char-set #\/)))

  ;; By combining O_NOFOLLOW and O_DIRECTORY, this procedure automatically
  ;; verifies that no components are symlinks.
  (define open-flags (logior O_CLOEXEC ; don't pass the port on to subprocesses
                             O_RDONLY  ;needed on the Hurd, harmless on Linux
                             O_NOFOLLOW ; don't follow symlinks
                             O_DIRECTORY)) ; reject anything not a directory

  (let loop ((components (string-tokenize directory not-slash))
             (root (open (if absolute? "/" ".") open-flags)))
    (match components
      ((head tail ...)
       (let retry ()
         ;; In the usual case, we expect HEAD to already exist.
         (match (catch 'system-error
                  (lambda ()
                    (openat root head open-flags))
                  (lambda args
                    (if (= ENOENT (system-error-errno args))
                        #false
                        (begin
                          (close-port root)
                          (apply throw args)))))
           ((? port? new-root)
            (close root)
            (loop tail new-root))
           (#false
            ;; If not, create it.
            (catch 'system-error
              (lambda _
                (if (null? tail)
                    (mkdirat root head bits)
                    (mkdirat root head)))
              (lambda args
                ;; Someone else created the directory.  Unexpected but fine.
                (unless (= EEXIST (system-error-errno args))
                  (close-port root)
                  (apply throw args))))
            (retry)))))
      (()
       (catch 'system-error
         (lambda ()
           (chown root (passwd:uid owner) (passwd:gid owner))
           (chmod root bits))
         (lambda args
           (close-port root)
           (apply throw args)))
       (close-port root)
       (values)))))

(define* (copy-account-skeletons home
                                 #:key
                                 (directory %skeleton-directory)
                                 uid gid)
  "Copy the account skeletons from DIRECTORY to HOME and make them writable.
When UID is an integer, make it the owner of all the files created except the
home directory; likewise for GID."
  (define (set-permission file)
    (when (or uid gid)
      (chown file (or uid -1) (or gid -1)))
    (make-file-writable file))

  (let ((files (scandir directory (negate dot-or-dot-dot?)
                        string<?)))
    (mkdir-p home)
    (for-each (lambda (file)
                (let ((target (string-append home "/" file)))
                  (copy-recursively (string-append directory "/" file)
                                    target
                                    #:log (%make-void-port "w"))
                  (for-each set-permission
                            (find-files target (const #t)
                                        #:directories? #t))))
              files)))

(define* (make-skeletons-writable home
                                  #:optional (directory %skeleton-directory))
  "Make sure that the files that have been copied from DIRECTORY to HOME are
owner-writable in HOME."
  (let ((files (scandir directory (negate dot-or-dot-dot?)
                        string<?)))
    (for-each (lambda (file)
                (let ((target (string-append home "/" file)))
                  (when (file-exists? target)
                    (make-file-writable target))))
              files)))

(define (duplicates lst)
  "Return elements from LST present more than once in LST."
  (let loop ((lst lst)
             (seen vlist-null)
             (result '()))
    (match lst
      (()
       (reverse result))
      ((head . tail)
       (loop tail
             (vhash-cons head #t seen)
             (if (vhash-assoc head seen)
                 (cons head result)
                 result))))))

(define (activate-users+groups users groups)
  "Make sure USERS (a list of user account records) and GROUPS (a list of user
group records) are all available."
  (define (make-home-directory user)
    (let ((home (user-account-home-directory user))
          (pwd  (getpwnam (user-account-name user))))
      (mkdir-p home)

      ;; Always set ownership and permissions for home directories of system
      ;; accounts.  If a service needs looser permissions on its home
      ;; directories, it can always chmod it in an activation snippet.
      (chown home (passwd:uid pwd) (passwd:gid pwd))
      (chmod home #o700)))

  (define system-accounts
    (filter (lambda (user)
              (and (user-account-system? user)
                   (user-account-create-home-directory? user)))
            users))

  ;; Allow home directories to be created under /var/lib.
  (mkdir-p "/var/lib")

  ;; Take same lock as libc's 'lckpwdf' (but without a timeout) while we read
  ;; and write the databases.  This ensures there's no race condition with
  ;; other tools that might be accessing it at the same time.
  (with-file-lock %password-lock-file
    (let-values (((groups passwd shadow)
                  (user+group-databases users groups)))
      (write-group groups)
      (write-passwd passwd)
      (write-shadow shadow)))

  ;; Home directories of non-system accounts are created by
  ;; 'activate-user-home'.
  (for-each make-home-directory system-accounts)

  ;; Turn shared home directories, such as /var/empty, into root-owned,
  ;; read-only places.
  (for-each (lambda (directory)
              (chown directory 0 0)
              (chmod directory #o555))
            (duplicates (map user-account-home-directory system-accounts))))

(define (activate-subuids+subgids subuids subgids)
  "Make sure SUBUIDS (a list of subid range records) and SUBGIDS (a list of
subid range records) are all available."

  ;; Take same lock as Shadow while we read
  ;; and write the databases.  This ensures there's no race condition with
  ;; other tools that might be accessing it at the same time.
  (with-file-lock "/etc/subgid.lock"
    (let-values (((subuid subgid)
                  (subuid+subgid-databases subuids subgids)))
      (write-subgid subgid)))

  (with-file-lock "/etc/subuid.lock"
    (let-values (((subuid subgid)
                  (subuid+subgid-databases subuids subgids)))
      (write-subuid subuid))))

(define (activate-user-home users)
  "Create and populate the home directory of USERS, a list of tuples, unless
they already exist."
  (define ensure-user-home
    (lambda (user)
      (let ((name         (user-account-name user))
            (home         (user-account-home-directory user))
            (create-home? (user-account-create-home-directory? user))
            (system?      (user-account-system? user)))
        ;; The home directories of system accounts are created during
        ;; activation, not here.
        (unless (or (not home) (not create-home?) system?
                    (directory-exists? home))
          (let* ((pw  (getpwnam name))
                 (uid (passwd:uid pw))
                 (gid (passwd:gid pw)))
            (mkdir-p home)
            (chmod home #o700)
            (copy-account-skeletons home
                                    #:uid uid #:gid gid)

            ;; It is important 'chown' be called after
            ;; 'copy-account-skeletons'.  Otherwise, a malicious user with
            ;; good timing could create a symlink in HOME that would be
            ;; dereferenced by 'copy-account-skeletons'.
            (chown home uid gid))))))

  (for-each ensure-user-home users))

(define* (canonicalize-path* file)
  "A safe version of `canonicalize-path' that warns rather than raises on errors.
`canonicalize-path' uses `realpath(2)', which can return various errors like
EINVAL, ELOOP, etc."
  (or (false-if-exception (canonicalize-path file))
      (begin
        (format (current-warning-port)
                "warning: could not canonicalize file `~a'; using as-is~%"
                file)
        file)))

(define (activate-etc etc)
  "Install ETC, a directory in the store, as the source of static files for
/etc."

  ;; /etc is a mixture of static and dynamic settings.  Here is where we
  ;; initialize it from the static part.

  (define (rm-f file)
    (false-if-exception (delete-file file)))

  (format #t "populating /etc from ~a...~%" etc)
  (mkdir-p "/etc")

  ;; Create the /etc/ssl -> /run/current-system/profile/etc/ssl symlink.  This
  ;; symlink, to a target outside of the store, probably doesn't belong in the
  ;; static 'etc' store directory.  However, if it were to be put there,
  ;; beware that if /run/current-system/profile/etc/ssl doesn't exist at the
  ;; time of activation (e.g. when installing a fresh system), the call to
  ;; 'file-is-directory?' below will fail because it uses 'stat', not 'lstat'.
  (rm-f "/etc/ssl")
  (symlink "/run/current-system/profile/etc/ssl" "/etc/ssl")

  (for-each (lambda (file)
              (let ((target (string-append "/etc/" file))
                    ;; Canonicalize the file names to resolve any symlinks, to
                    ;; ensure /etc/localtime points to a timezone data file in
                    ;; the store containing the timezone name.  This is done
                    ;; for compatibility with software expecting this systemd
                    ;; convention to be followed.
                    (source (canonicalize-path* (string-append etc "/" file))))
                (rm-f target)
                (cond ((string=? (basename target) "sudoers")
                       (begin
                         ;; /etc/sudoers must be a regular file.
                         (copy-file source target)
                         ;; XXX: dirty hack to meet sudo's expectations
                         (chmod target #o440)))
                      ((string=? (basename target) "hosts")
                       ;; /etc/hosts must be a regular file, as some software
                       ;; like vpn-slice expect to be able to write to it.
                       (copy-file source target))
                      (else             ;usual case
                       (symlink source target)))))
            (scandir etc (negate dot-or-dot-dot?)
                     ;; The default is 'string-locale<?', but we don't have
                     ;; it when run from the initrd's statically-linked
                     ;; Guile.
                     string<?)))

(define %setuid-directory
  ;; Place where setuid programs used to be stored.  It exists for backwards
  ;; compatibility & will be removed.  Use %PRIVILEGED-PROGRAM-DIRECTORY instead.
  "/run/setuid-programs")

(define %privileged-program-directory
  ;; Place where privileged copies of programs are stored.
  "/run/privileged/bin")

(define (activate-privileged-programs programs libcap)
  "Turn PROGRAMS, a list of file privileged-programs records, into privileged
copies stored under %PRIVILEGED-PROGRAM-DIRECTORY, using LIBCAP's setcap(8)
binary if needed."
  (define (ensure-empty-directory directory)
    (if (file-exists? directory)
        (for-each (compose delete-file
                           (cut string-append directory "/" <>))
                  (scandir directory
                           (lambda (file)
                             (not (member file '("." ".."))))
                           string<?))
        (mkdir-p directory))    )

  (define (make-privileged-program program setuid? setgid? uid gid capabilities)
    (let ((target (string-append %privileged-program-directory
                                 "/" (basename program)))
          (mode (+ #o0555                   ; base permissions
                   (if setuid? #o4000 0)    ; setuid bit
                   (if setgid? #o2000 0)))) ; setgid bit
      (copy-file program target)
      (chown target uid gid)
      (chmod target mode)
      (when (and capabilities libcap)
        (system* (string-append libcap "/sbin/setcap")
                 "-q" capabilities target))))

  (define (make-deprecated-wrapper program)
    ;; This will eventually become a script that warns on usage, then vanish.
    (symlink (string-append %privileged-program-directory
                            "/" (basename program))
             (string-append %setuid-directory
                            "/" (basename program))))

  (format #t "setting up privileged programs in '~a'...~%"
          %privileged-program-directory)
  (ensure-empty-directory %privileged-program-directory)
  (ensure-empty-directory %setuid-directory)

  (for-each (lambda (program)
              (catch 'system-error
                (lambda ()
                  (let* ((program-name (privileged-program-program program))
                         (setuid?      (privileged-program-setuid? program))
                         (setgid?      (privileged-program-setgid? program))
                         (user         (privileged-program-user program))
                         (group        (privileged-program-group program))
                         (capabilities (privileged-program-capabilities program))
                         (uid (match user
                                ((? string?) (passwd:uid (getpwnam user)))
                                ((? integer?) user)))
                         (gid (match group
                                ((? string?) (group:gid (getgrnam group)))
                                ((? integer?) group))))
                    (make-privileged-program program-name
                                             setuid? setgid? uid gid
                                             capabilities)
                    (make-deprecated-wrapper program-name)))
                (lambda args
                  ;; If we fail to create a privileged program, better keep going
                  ;; so that we don't leave %PRIVILEGED-PROGRAM-DIRECTORY empty
                  ;; or half-populated.  This can happen if PROGRAMS contains
                  ;; incorrect file names: <https://bugs.gnu.org/38800>.
                  (format (current-error-port)
                          "warning: failed to privilege ~s: ~a~%"
                          (privileged-program-program program)
                          (strerror (system-error-errno args))))))
            programs))

(define (activate-special-files special-files)
  "Install the files listed in SPECIAL-FILES.  Each element of SPECIAL-FILES
is a pair where the first element is the name of the special file and the
second element is the name it should appear at, such as:

  ((\"/bin/sh\" \"/gnu/store/…-bash/bin/sh\")
   (\"/usr/bin/env\" \"/gnu/store/…-coreutils/bin/env\"))
"
  (define install-special-file
    (match-lambda
      ((target file)
       (let ((pivot (string-append target ".new")))
         (mkdir-p (dirname target))
         (symlink file pivot)
         (rename-file pivot target)))))

  (for-each install-special-file special-files))

(define (activate-modprobe modprobe)
  "Tell the kernel to use MODPROBE to load modules."

  ;; If the kernel was built without loadable module support, this file is
  ;; unavailable, so check for its existence first.
  (when (file-exists? "/proc/sys/kernel/modprobe")
    (call-with-output-file "/proc/sys/kernel/modprobe"
      (lambda (port)
        (display modprobe port)))))

(define (activate-firmware directory)
  "Tell the kernel to look for device firmware under DIRECTORY.  This
mechanism bypasses udev: it allows Linux to handle firmware loading directly
by itself, without having to resort to a \"user helper\"."

  ;; If the kernel was built without firmware loading support, this file
  ;; does not exist.  Do nothing in that case.
  (let ((firmware-path "/sys/module/firmware_class/parameters/path"))
    (when (file-exists? firmware-path)
      (call-with-output-file firmware-path
        (lambda (port)
          (display directory port))))))

(define (activate-ptrace-attach)
  "Allow users to PTRACE_ATTACH their own processes.

This works around a regression introduced in the default \"security\" policy
found in Linux 3.4 onward that prevents users from attaching to their own
processes--see Yama.txt in the Linux source tree for the rationale.  This
sounds like an unacceptable restriction for little or no security
improvement."
  (let ((file "/proc/sys/kernel/yama/ptrace_scope"))
    (when (file-exists? file)
      (call-with-output-file file
        (lambda (port)
          (display 0 port))))))


(define %current-system
  ;; The system that is current (a symlink.)  This is not necessarily the same
  ;; as the system we booted (aka. /run/booted-system) because we can re-build
  ;; a new system configuration and activate it, without rebooting.
  "/run/current-system")

(define (boot-time-system)
  "Return the 'gnu.system' argument passed on the kernel command line."
  (find-long-option "gnu.system" (if (string-contains %host-type "linux-gnu")
                                   (linux-command-line)
                                   (command-line))))

(define* (activate-current-system
          #:optional (system (or (getenv "GUIX_NEW_SYSTEM")
                                 (boot-time-system))))
  "Atomically make SYSTEM the current system."
  ;; The 'GUIX_NEW_SYSTEM' environment variable is used as a way for 'guix
  ;; system reconfigure' to pass the file name of the new system.

  (format #t "making '~a' the current system...~%" system)

  (mkdir-p "/run")
  ;; Atomically make SYSTEM current.
  (let ((new (string-append %current-system ".new")))
    (when (file-exists? new)
      (delete-file new))
    (symlink system new)
    (rename-file new %current-system)))

;;; activation.scm ends here
