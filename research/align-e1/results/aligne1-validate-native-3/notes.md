# aligne1-validate-native-3 — notes

INVALID attempt (retained immutable): harness bug 3 — driver did not pass
--file-bytes to the bench, so the bench enforced its 512 MiB default
against the 128 MiB src; every run failed the bench-side same-work gate
(bytes_copied != file size, exit 3). 48 runs, 48 gate errors. The
fail-closed gate caught a real configuration bug. Superseded by
aligne1-validate-native-6.