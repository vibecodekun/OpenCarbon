"""Emit Ghidra-safe names for the SM83 (Game Boy CPU) primary opcode table."""
R = ['B','C','D','E','H','L','mHL','A']
RP = ['BC','DE','HL','SP']; RP2 = ['BC','DE','HL','AF']
CC = ['NZ','Z','NC','C']
ALU = ['ADD_A','ADC_A','SUB','SBC_A','AND','XOR','OR','CP']
def name(op):
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    if x == 0:
        if z == 0:
            return ['NOP','LD_mnn_SP','STOP','JR_e'][y] if y < 4 else f'JR_{CC[y-4]}_e'
        if z == 1: return f'LD_{RP[p]}_nn' if q == 0 else f'ADD_HL_{RP[p]}'
        if z == 2:
            return (['LD_mBC_A','LD_mDE_A','LD_mHLi_A','LD_mHLd_A'] if q == 0 else ['LD_A_mBC','LD_A_mDE','LD_A_mHLi','LD_A_mHLd'])[p]
        if z == 3: return f'{"INC" if q == 0 else "DEC"}_{RP[p]}'
        if z == 4: return f'INC_{R[y]}'
        if z == 5: return f'DEC_{R[y]}'
        if z == 6: return f'LD_{R[y]}_n'
        return ['RLCA','RRCA','RLA','RRA','DAA','CPL','SCF','CCF'][y]
    if x == 1: return 'HALT' if op == 0x76 else f'LD_{R[y]}_{R[z]}'
    if x == 2: return f'{ALU[y]}_{R[z]}'
    if z == 0: return [f'RET_NZ','RET_Z','RET_NC','RET_C','LDH_mn_A','ADD_SP_e','LDH_A_mn','LD_HL_SPe'][y]
    if z == 1: return f'POP_{RP2[p]}' if q == 0 else ['RET','RETI','JP_HL','LD_SP_HL'][p]
    if z == 2: return [f'JP_NZ_nn','JP_Z_nn','JP_NC_nn','JP_C_nn','LD_mFFC_A','LD_mnn_A','LD_A_mFFC','LD_A_mnn'][y]
    if z == 3: return ['JP_nn','CB_prefix','ILLEGAL','ILLEGAL','ILLEGAL','ILLEGAL','DI','EI'][y]
    if z == 4: return [f'CALL_NZ_nn','CALL_Z_nn','CALL_NC_nn','CALL_C_nn','ILLEGAL','ILLEGAL','ILLEGAL','ILLEGAL'][y]
    if z == 5: return f'PUSH_{RP2[p]}' if q == 0 else ['CALL_nn','ILLEGAL','ILLEGAL','ILLEGAL'][p]
    if z == 6: return f'{ALU[y]}_n'
    return f'RST_{y*8:02X}'
if __name__ == '__main__':
    import sys
    seen = {}
    for line in open(r'C:\opencarbon\ghidra\opcode_table.txt'):
        op, addr = line.split(); op = int(op, 16)
        seen.setdefault(addr, []).append(op)
    for addr, ops in seen.items():
        if len(ops) > 1: print(f'{addr}\top_illegal\tF\tshared by {",".join("%02X" % o for o in ops)}')
        else: print(f'{addr}\top_{ops[0]:02X}_{name(ops[0])}\tF')
