# Offline Steam UI scaffold

O1 implements only standard-library Python contracts in `model.py` and three
fixed mock library entries. IDs are local identifiers, not Steam app IDs or
proof of ownership. No UI, QR code, runtime adapter, install or launch is
implemented in this order.

`OfflineSteamBackend.begin_login()` enters `MOCK_PENDING`;
`approve_mock_login()` explicitly approves the simulated session. Cancel clears
the session, and a new backend starts signed out. Only a mock signed-in session
can list the fixture library. Other login flow names (including `qr` and
`refresh_token`) return `UNAVAILABLE`; `download()` always returns
`UNAVAILABLE`. Install states distinguish not installed, future mock fixture
readiness, and unavailable operations. `SteamBackend` defines the future
connector boundary without supplying a live implementation.

No passwords, token fields, credential objects, network calls or state writes
exist. Mock approval establishes no device/API authorization. A future real
adapter would use QR approval and refresh-token authentication with João's own
account, retrieve the owned library and check entitlements before depot
downloads. Refresh tokens must be held by a platform credential store and
redacted from UI, logs and errors; this scaffold implements no secret storage.

The planned shell uses a controller-friendly grid, focus navigation and explicit
Install/Launch actions. WinUI 3 XAML targets desktop Windows. UWP needs a distinct
compatible XAML/project/API adaptation and documented capability validation;
there is no drop-in port or Xbox build/device claim here. The future launch
adapter must route owned hello.exe through the existing `run_via_wine` launcher
and `x64/uwp-host` harness, preserving its acceptance checks and blockers.
Linux subprocess orchestration cannot establish an in-process UWP launch model.

Validation from the repository root:

```sh
python3 -m unittest discover -s x64/ui/tests -p 'test_model.py' -v
make -C x64/uwp-host all
git diff --check
```

Proof classification: model tests and documentation are HOST-ONLY. The parent
command builds/checks uwp-host artifacts; it does not execute the uwp-host
runtime, our launcher or stock Wine. O1 supplies no launch routing proof or
video; visible UI/capture belongs to later orders. R4H's wineboot-attempt
acceptance blocker remains unresolved.
