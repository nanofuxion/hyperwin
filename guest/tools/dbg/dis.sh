#!/bin/bash
# dis.sh <module-file-in-dwmfiles> <pdbname> <symbol> [bytes]
M=$1; PDBN=$2; SYM=$3; N=${4:-0x300}
O=/opt/homebrew/opt/llvm/bin
PDB=$(ls /tmp/dwmre/sym/$PDBN.pdb/*/$PDBN.pdb)
LINE=$($O/llvm-pdbutil dump --publics $PDB | grep -A1 "\`$SYM\`" | grep addr | head -1)
OFF=$(echo "$LINE" | sed -n 's/.*addr = 0001:\([0-9]*\).*/\1/p')
[ -z "$OFF" ] && { echo "no sym $SYM ($LINE)"; exit 1; }
cd /tmp/dwmre/dwmfiles
B=$($O/llvm-readobj --file-headers $M | grep -i ImageBase | head -1 | awk '{print $2}')
TV=$($O/llvm-readobj --sections $M | awk '/Name: .text/{f=1} f&&/VirtualAddress/{print $2; exit}')
S=$(python3 -c "print(hex($B+$TV+$OFF))"); E=$(python3 -c "print(hex($B+$TV+$OFF+$N))")
$O/llvm-objdump -d --no-show-raw-insn --start-address=$S --stop-address=$E $M | tail -n +6
