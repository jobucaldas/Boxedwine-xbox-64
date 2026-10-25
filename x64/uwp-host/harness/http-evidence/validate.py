"""Validate completed probes and qualify the external-server transport path."""
from pathlib import Path
import re
import sys


def validate(e):
    for mode in ('stock', 'uwph'):
        log = (e / f'{mode}.log').read_text()
        rc = int((e / f'{mode}.exit').read_text())
        fail = re.search(r'^FAIL function=\S+ GetLastError=\d+ \(0x[0-9a-f]+\)$', log, re.M)
        if mode == 'uwph' and fail:
            assert rc == 1, f'uwph FAIL must exit 1, got {rc}'
        else:
            assert rc == 0, f'{mode} must exit 0, got {rc}'
            assert re.search(r'^HTTP 200$', log, re.M), f'{mode}: missing HTTP 200'
            assert re.search(r'^BODY bytes=\d+$', log, re.M), f'{mode}: incomplete body'
            assert re.search(r'^COMPLETE exit=0$', log, re.M), f'{mode}: incomplete execution'
            assert not fail, f'{mode}: unexpected FAIL'
    client = (e / 'uwph.strace').read_text()
    server = (e / 'uwph-server.strace').read_text()
    assert 'execve(' in client and '/run_via_wine"' in client, 'missing launcher exec'
    assert 'http_probe.exe' in client, 'missing probe exec'
    counters = list(e.glob('uwph.counters.*'))
    assert counters, 'missing shim counters'
    assert any(re.search(r'^total read [1-9]\d* ', p.read_text(), re.M) for p in counters), 'missing shim activity'
    connections = [line for line in server.splitlines() if 'connect(' in line and 'htons(443)' in line]
    # A successful run must provide actual network-path evidence, not a marker.
    success = int((e / 'uwph.exit').read_text()) == 0
    summary = ['Qualified launcher/shim comparison only; not uwph HTTP transport or Xbox/UWP acceptance.']
    if success:
        assert connections, 'missing external-server HTTPS connect evidence'
        transfers = [line for line in client.splitlines() if 'SCM_RIGHTS' in line and 'TCP:' in line]
        assert transfers, 'missing server-to-client TCP descriptor transfer'
        traffic = [line for line in client.splitlines() if re.search(r'(sendto|recvfrom|sendmsg|recvmsg)\(\d+<TCP:', line) and ':443]' in line]
        assert traffic, 'missing client native HTTPS socket activity'
        pid = traffic[0].split()[0]
        report = e / f'uwph.counters.{pid}'
        assert report.exists(), 'missing probe-process shim report'
        summary += ['External stock wineserver owns connect; client receives TCP fd via SCM_RIGHTS and uses native socket syscalls.',
                    *connections, transfers[0], traffic[0],
                    f'Client PID {pid}: ' + ', '.join(line for line in report.read_text().splitlines() if line.startswith(('trace connect ', 'trace sendmsg ', 'trace recvmsg ')))]
    else:
        summary += ['Probe failed with exit 1 and a Windows API FAIL record; routing remains unproven.',
                    *connections]
    (e / 'routing-evidence.txt').write_text('\n'.join(summary) + '\n')
    print('PASS: completed comparison and shim/path evidence validated; transport acceptance remains BLOCKED')


if __name__ == '__main__':
    validate(Path(sys.argv[1]))
