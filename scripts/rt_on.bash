#! /usr/bin/env bash
echo "Setting CPU governor to performance mode"
echo -n performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

echo "Turning off SMT"
echo off | sudo tee /sys/devices/system/cpu/smt/control

echo "Setting swapiness to 10"
sudo sysctl vm.swappiness=10

echo "Locking USB IRQ to cpu 12"
echo 12 | sudo tee /proc/irq/39/smp_affinity_list 

echo "Disabling C2 and C3 states"
for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
echo 1 | sudo tee "$cpu/cpuidle/state2/disable" >/dev/null
echo 1 | sudo tee "$cpu/cpuidle/state3/disable" >/dev/null
done 

echo "double check pls"

echo "CPU governor:"
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 

echo "SMT:"
cat /sys/devices/system/cpu/smt/control

echo "Swapiness:"
cat /proc/sys/vm/swappiness

echo "USB IRQ:"
cat /proc/irq/39/smp_affinity_list

echo "C2 and C3 states:"
for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
echo "$cpu"
paste $cpu/cpuidle/state*/name $cpu/cpuidle/state*/disable 2>/dev/null
done 


