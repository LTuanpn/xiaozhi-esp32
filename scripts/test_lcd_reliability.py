"""Serial MCP smoke test for the LCD board. Does not flash or change credentials."""
import argparse
import json
import time
from pathlib import Path
import serial

EMOTIONS = (
    'happy sad angry laugh cry wink sleepy surprised skeptical smug furious love dizzy '
    'scared excited bored pout music scanning thinking shy neutral matrix loading battery '
    'overheat reboot target side_eye facepalm innocent jealous scheming disgusted whisper '
    'freeze sneeze waterfall headache drunk exhausted zen jawdrop paranoid question idea '
    'hypno money heartbroken tongue party ninja game_over fire thunder rain sunny sleepwalk '
    'angel fire_fury tongue_out playful_wink confused_question shocked_jawdrop matrix_glitch '
    'idea_eureka target_locked money_eyes party_celebrate low_battery zen_meditation'
).split()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port')
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--alarms', action='store_true', help='Set a 2-second audible timer')
    parser.add_argument('--qr', action='store_true', help='Leave a test URL QR visible for phone verification')
    args = parser.parse_args()
    connection = serial.Serial(port=None, baudrate=115200, timeout=.15)
    connection.dtr = connection.rts = False
    connection.port = args.port
    connection.open()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    request_id = 10000
    with connection, args.log.open('w', encoding='utf-8') as log:
        def read_line():
            line = connection.readline().decode('utf-8', errors='replace')
            if line:
                log.write(line)
                log.flush()
                if any(marker in line for marker in ('Guru Meditation', 'panic_abort', 'Stack canary watchpoint', 'assert failed')):
                    raise RuntimeError('Firmware panic; inspect serial log')
            return line

        def pump(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                read_line()

        def call(name, arguments, expect_error=False):
            nonlocal request_id
            request_id += 1
            request = {'jsonrpc':'2.0', 'id':request_id, 'method':'tools/call',
                       'params':{'name':name, 'arguments':arguments}}
            connection.write((json.dumps(request, ensure_ascii=False)+'\n').encode('utf-8'))
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                line = read_line()
                if 'MCPCLI ' not in line: continue
                try:
                    response = json.JSONDecoder().raw_decode(line.split('MCPCLI ',1)[1])[0]
                except ValueError:
                    continue
                if response.get('id') != request_id: continue
                failed = 'error' in response or response.get('result',{}).get('isError',False)
                if failed != expect_error: raise RuntimeError(f'Unexpected tool response: {response}')
                return response
            raise TimeoutError(f'No serial MCP response for {name}')

        pump(1)
        for emotion in EMOTIONS:
            call('self.screen.set_emotion', {'emotion':emotion, 'duration_seconds':1})
            pump(.15)
        call('self.screen.set_emotion', {'emotion':'unsupported-face'}, expect_error=True)
        call('self.screen.set_emotion', {'emotion':'neutral', 'duration_seconds':1})
        print(f'PASS {len(EMOTIONS)} emotion names and invalid-name rejection (visual appearance not verified)')
        if args.alarms:
            call('self.timer.set_countdown', {'seconds':2, 'label':'TEST TIMER'})
            pump(9)
            call('self.timer.set_countdown', {'seconds':2, 'label':'CANCELLED'})
            call('self.timer.set_countdown', {'seconds':0})
            pump(3)
            print('Timer expiry/cancel sequence completed; verify sound and Local alarm log')
        if args.qr:
            connection.write(b'qr https://example.com\n')
            pump(3)
            print('Test QR https://example.com displayed request sent; scan with a real phone')
        pump(2)
        print('Serial run completed without an observed panic; log:', args.log)


if __name__ == '__main__':
    main()
