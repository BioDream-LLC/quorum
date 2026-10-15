# Synthetic rulebooks for timing cbc: `python3 gen.py <groups>` writes one with 3 readings per group.
# Each group is a fault, a check that rebuts it and a failed-sensor reading that undercuts it; half the faults stop
# the machine, the rest raise an alarm. Used by compiler_threads_same and for the timings in the README (Limits).
import sys
k=int(sys.argv[1])  # groups of 3 readings
L=[f'casebook scale{k*3} v1','source "synthetic scaling test" dated 2026-10-14 by "quorum tests"']
safe=[];alarm=[]
for i in range(k):
    L+=[f'observe f{i} "fault {i}"',f'observe c{i} "check {i}"',f'observe s{i} "sensor {i} failed"',
        f'claim fault{i} "fault {i} present" needs f{i}', f'c{i} rebuts fault{i}', f's{i} undercuts fault{i}',
        f'claim blind{i} "sensor {i} cannot be trusted" needs s{i}']
    (safe if i%2==0 else alarm).append(f'fault{i}'); alarm.append(f'blind{i}')
L+=['action run idle',f'action stop safe when any {" ".join(safe)}',f'action alarm when any {" ".join(alarm)}',
    'prefer stop over alarm over run']
print('\n'.join(L))
