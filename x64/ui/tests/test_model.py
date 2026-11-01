"""Behavior checks for offline contracts; no Wine or network execution."""

import sys
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from model import AuthState, InstallState, LibraryState, OfflineSteamBackend


class OfflineModelTests(unittest.TestCase):
    def test_mock_approval_and_cancel_control_library_access(self):
        backend = OfflineSteamBackend()
        self.assertEqual(backend.library().state, LibraryState.SIGNED_OUT)
        self.assertEqual(backend.begin_login(), AuthState.MOCK_PENDING)
        self.assertEqual(backend.library().games, ())
        self.assertEqual(backend.cancel_login(), AuthState.SIGNED_OUT)
        self.assertEqual(backend.approve_mock_login(), AuthState.SIGNED_OUT)
        backend.begin_login()
        backend.approve_mock_login()
        self.assertEqual(backend.library().state, LibraryState.MOCK)
        backend.cancel_login()
        self.assertEqual(backend.library().games, ())

    def test_library_has_stable_local_ids_and_only_mock_entries(self):
        def signed_in_library():
            backend = OfflineSteamBackend()
            backend.begin_login()
            backend.approve_mock_login()
            return backend.library().games

        games = signed_in_library()
        self.assertEqual([game.id for game in games],
                         ["hello-demo", "mock-orbit", "mock-garden"])
        self.assertEqual(games, signed_in_library())
        self.assertTrue(all(game.mock_only for game in games))
        self.assertTrue(all(game.install_state == InstallState.NOT_INSTALLED
                            for game in games))

    def test_live_authentication_and_download_are_explicitly_unavailable(self):
        backend = OfflineSteamBackend()
        for flow in ("qr", "refresh_token", "live"):
            self.assertEqual(backend.begin_login(flow), AuthState.UNAVAILABLE)
            self.assertEqual(backend.approve_mock_login(), AuthState.UNAVAILABLE)
            self.assertEqual(backend.library().state, LibraryState.UNAVAILABLE)
        for game_id in ("hello-demo", "unknown"):
            self.assertEqual(backend.download(game_id), InstallState.UNAVAILABLE)

    def test_sessions_do_not_persist_credentials_tokens_or_login(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.dict("os.environ", {"HOME": directory,
                                          "XDG_CONFIG_HOME": directory,
                                          "XDG_DATA_HOME": directory}):
                # Reject Python file writes anywhere, including outside HOME.
                import builtins
                original_open = builtins.open

                def read_only_open(file, mode="r", *args, **kwargs):
                    self.assertFalse(any(flag in mode for flag in "wax+"))
                    return original_open(file, mode, *args, **kwargs)

                with patch("builtins.open", side_effect=read_only_open), \
                     patch("io.open", side_effect=read_only_open):
                    backend = OfflineSteamBackend()
                    backend.begin_login()
                    backend.approve_mock_login()
                    backend.library()
                    backend.download("hello-demo")
                    backend.begin_login("refresh_token")
                    backend.cancel_login()
                    self.assertEqual(OfflineSteamBackend().auth_state,
                                     AuthState.SIGNED_OUT)
                self.assertEqual(list(Path(directory).rglob("*")), [])


if __name__ == "__main__":
    unittest.main()
