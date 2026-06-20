import subprocess, time
cmd = ['.\\build\\minichess-ubgi.exe']
proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
inputs = ['uci\n', 'isready\n', 'position startpos\n', 'go depth 12\n', 'stop\n', 'quit\n']
proc.stdin.writelines(inputs)
proc.stdin.flush()
time.sleep(1)
try:
    out, err = proc.communicate(timeout=5)
except subprocess.TimeoutExpired:
    proc.kill()
    out, err = proc.communicate()
print('STDOUT:')
print(out)
print('STDERR:')
print(err)
print('RETURN', proc.returncode)
