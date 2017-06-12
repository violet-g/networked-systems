import sys

addr = []
for line in sys.stdin:
  line = line.strip().split(" ")
  if line[2] == '*':
    continue
  if line[0] == 'traceroute':
    addr.append('do not process')
  else:
    addr.append(line[2])

#print addr

addr_pairs = []
for i in range(1,len(addr)-1):
  if (addr[i] == 'do not process' or addr[i+1] == 'do not process'):
    continue
  addr_pairs.append('"' + addr[i] + '" -- "' + addr[i + 1] + '"')
print('\n'.join(addr_pairs))
