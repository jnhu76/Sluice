import unittest

from grep_trace_oracle import successful_creations


class SuccessfulCreationTest(unittest.TestCase):
    def test_failed_attempts_are_not_creations(self):
        trace = """123 clone3({flags=CLONE_THREAD}, 88) = -1 ENOSYS (Function not implemented)
123 clone(child_stack=NULL, flags=CLONE_THREAD) = -1 EAGAIN (Resource temporarily unavailable)
"""
        self.assertEqual(successful_creations(trace), 0)

    def test_unfinished_attempt_is_not_a_creation(self):
        self.assertEqual(successful_creations("123 clone(flags=CLONE_THREAD <unfinished ...>\n"), 0)

    def test_successful_completed_and_resumed_returns_count_once(self):
        trace = """123 clone3({flags=CLONE_THREAD}, 88) = -1 ENOSYS (Function not implemented)
123 clone(child_stack=NULL, flags=CLONE_THREAD) = 124
123 clone(child_stack=NULL, flags=CLONE_THREAD <unfinished ...>
123 <... clone resumed>) = 125
124 +++ exited with 0 +++
"""
        self.assertEqual(successful_creations(trace), 2)

    def test_non_clone_syscalls_do_not_count(self):
        self.assertEqual(successful_creations("123 openat(AT_FDCWD, \"clone\", O_RDONLY) = 124\n"), 0)


if __name__ == "__main__":
    unittest.main()
