"""Offline Steam UI contracts. No network, credentials, or persistent state."""

from dataclasses import dataclass
from enum import Enum
import json
from pathlib import Path
from typing import Protocol


class AuthState(str, Enum):
    SIGNED_OUT = "signed_out"
    MOCK_PENDING = "mock_pending"
    MOCK_SIGNED_IN = "mock_signed_in"
    UNAVAILABLE = "unavailable"


class LibraryState(str, Enum):
    SIGNED_OUT = "signed_out"
    MOCK = "mock"
    UNAVAILABLE = "unavailable"


class InstallState(str, Enum):
    NOT_INSTALLED = "not_installed"
    MOCK_READY = "mock_ready"
    UNAVAILABLE = "unavailable"


@dataclass(frozen=True)
class Game:
    id: str
    title: str
    mock_only: bool = True
    install_state: InstallState = InstallState.NOT_INSTALLED


@dataclass(frozen=True)
class Library:
    state: LibraryState
    games: tuple[Game, ...] = ()


class SteamBackend(Protocol):
    """Future connector boundary; live implementations need separate review.

    Authentication takes a flow name, never a password/token from this UI.
    A future adapter owns approval and platform credential-store access.
    """

    def begin_login(self, flow: str = "mock") -> AuthState: ...
    def cancel_login(self) -> AuthState: ...
    def library(self) -> Library: ...
    def download(self, game_id: str) -> InstallState: ...


class OfflineSteamBackend:
    """Explicit mock approval only; downloads always return unavailable.

    MOCK_READY is reserved for a later owned-fixture runtime adapter. This
    model never installs content or launches an executable.
    """

    def __init__(self) -> None:
        self.auth_state = AuthState.SIGNED_OUT

    def begin_login(self, flow: str = "mock") -> AuthState:
        self.auth_state = (AuthState.MOCK_PENDING if flow == "mock"
                           else AuthState.UNAVAILABLE)
        return self.auth_state

    def approve_mock_login(self) -> AuthState:
        if self.auth_state == AuthState.MOCK_PENDING:
            self.auth_state = AuthState.MOCK_SIGNED_IN
        return self.auth_state

    def cancel_login(self) -> AuthState:
        self.auth_state = AuthState.SIGNED_OUT
        return self.auth_state

    def library(self) -> Library:
        if self.auth_state == AuthState.UNAVAILABLE:
            return Library(LibraryState.UNAVAILABLE)
        if self.auth_state != AuthState.MOCK_SIGNED_IN:
            return Library(LibraryState.SIGNED_OUT)
        rows = json.loads(Path(__file__).with_name("mock_library.json").read_text())
        games = tuple(Game(row["id"], row["title"]) for row in rows)
        if len({game.id for game in games}) != len(games):
            raise ValueError("duplicate mock game ID")
        return Library(LibraryState.MOCK, games)

    def download(self, game_id: str) -> InstallState:
        return InstallState.UNAVAILABLE
