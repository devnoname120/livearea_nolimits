#!/usr/bin/env python3
"""Compiled recovery preload, native PAF Module lifetime and recovery decision.

OS, strings/maps, scheduling and rendering services are modeled. Native recovery
initialization executes in a separate Unicorn context with the exact patch bytes
installed by the compiled plugin. No Vita or real database writes occur.
Usage: test_recovery_preload_arm.py PLUGIN_ELF EVIDENCE_ROOT [--database DB ...]
"""
from pathlib import Path
import argparse,json,sqlite3,struct
from unicorn import UC_HOOK_CODE
from unicorn import arm_const as R
from arm_helpers import elf,machine,word,check_encoder,STACK,RETURN,HEAP
import test_recovery_firmware as rec

REC, RECDATA, SHELL, SHELLDATA = 0x81400000,0x8143F000,0x81800000,0x82100000
PARAM, NAME, FILENAME, PLUGIN, OWNER, INFO = [HEAP+x for x in (0x100,0x200,0x240,0x500,0x700,0x800)]

class Trial:
    def __init__(self,plugin,root,profile,counts):
        self.u=machine(plugin);self.sym=plugin[2];self.root=root;self.profile=profile;self.counts=counts
        self.paf,self.pdata=(0x83200E90,0x811607E0) if profile==2 else (0x81000000,0x81301000)
        files=['retail360-paf.text.bin','retail365-paf.text.bin','ptel360-paf.text.bin']
        self.paftext=(root/'worktrees/missing-apps-recovery/build/cache-parity-evidence'/files[profile]).read_bytes()
        if profile==2:self.u.mem_map(self.paf&~4095,0x302000);self.u.mem_write(self.paf,self.paftext)
        else:self.u.mem_map(self.paf+0x40000,0x2C1000);self.u.mem_write(self.paf+0x40000,self.paftext[0x40000:])
        self.u.mem_map(self.pdata&~4095,0x18000)
        self.rectext=(root/f'worktrees/missing-apps-recovery/build/recovery-evidence/retail{365 if profile==1 else 360}-recovery.text.bin').read_bytes()
        self.u.mem_map(REC,0x43000);self.u.mem_write(REC,self.rectext)
        self.offsets=[0x99,0x9B,0x15F,0x167,0x207]
        runtime=(root/f'worktrees/missing-apps-recovery/build/recovery-evidence/retail{365 if profile==1 else 360}-recovery.analysis.elf').read_bytes()
        eh=struct.unpack_from('<16sHHIIIIIHHHHHH',runtime);segments=[struct.unpack_from('<8I',runtime,eh[5]+i*eh[9]) for i in range(eh[10])];data_segment=[x for x in segments if x[0]==1][1]
        for offset in range(0x18,0x38,4):
            value=struct.unpack_from('<I',runtime,data_segment[1]+offset)[0]
            if rec.BASE<=value<rec.BASE+len(self.rectext):value+=REC-rec.BASE
            word(self.u,RECDATA+offset,value)
        for i,x in enumerate(self.offsets):word(self.u,RECDATA+4*i,REC+x)
        shells=[root/'firmware-evidence/3.60/SceShell.text.bin',root/'firmware-evidence/3.65/SceShell.text.bin',root/'release-v1.9.0-evidence/ptel360-shell.text.bin']
        self.shell=shells[profile].read_bytes();self.u.mem_map(SHELL,(len(self.shell)+4095)&~4095);self.u.mem_write(SHELL,self.shell)
        self.u.mem_map(SHELLDATA,0x10000)
        self.nid=[0x0552F692,0x5549BF1F,0xEAB89D5C][profile]
        self.rnid=0x3F76E38F if profile==1 else 0xC1F30F67
        self.pnid=0x73F90499 if profile==1 else 0xCD679177
        self.import_load=[0x45CA50,0x45CE98,0x452C08][profile]
        self.heap=HEAP+0x2000;self.live={};self.cache={};self.mutex={};self.available=False;self.start_calls=0;self.stop_calls=0
        self.callbacks=[];self.logs=[];self.patches={};self.injections=0;self.fail_at=-1;self.fail_release=-1;self.kernel_failure=0
        self.native_inits=0;self.finished=0;self.init_cpu=None;self.expected_flags=0x80000;self.omit_interface=False
        self.cachehead=self.alloc(32);word(self.u,self.pdata+0xDEE0,self.cachehead)
        framework=self.alloc(128);word(self.u,self.pdata+0x194,framework)
        self.u.mem_write(NAME,b'dbrecovery_plugin\0');self.u.mem_write(FILENAME,b'vs0:vsh/common/dbrecovery_plugin.suprx\0')
        self.u.mem_write(PARAM,bytes(160));word(self.u,PARAM,NAME);word(self.u,PARAM+4,17)
        word(self.u,PARAM+104,FILENAME);word(self.u,PARAM+108,len(b'vs0:vsh/common/dbrecovery_plugin.suprx'))
        word(self.u,PARAM+116,1);word(self.u,PARAM+120,1)
        word(self.u,INFO,0x1B8);word(self.u,INFO+0x15C,SHELL);word(self.u,INFO+0x160,len(self.shell))
        self.mov(SHELL+0xC12,0xF240,1,(SHELL+0x1BFF)&65535);self.mov(SHELL+0xC18,0xF2C0,1,(SHELL+0x1BFF)>>16)
        # The original finish callback is exercised separately below; this
        # harness service stands in for its externally visible completion.
        self.relocate_ready()
        self.u.hook_add(UC_HOOK_CODE,self.step)
        self.names={addr&~1:name for name,addr in self.sym.items()}
        self.active_stop_return=[]

    def rd(self,a):return struct.unpack('<I',self.u.mem_read(a,4))[0]
    def string(self,a):return bytes(self.u.mem_read(a,256)).split(b'\0',1)[0].decode()
    def alloc(self,size):
        a=self.heap;self.heap=(a+max(size,1)+15)&~15;assert self.heap<HEAP+0xE000
        self.u.mem_write(a,bytes(max(size,1)));self.live[a]=size;return a
    def free(self,a):
        if not a:return
        assert a in self.live,hex(a);del self.live[a]
    def mov(self,a,op,reg,value):
        x=op|(value>>12)|((value>>1)&0x400);y=((value<<4)&0x7000)|(reg<<8)|(value&255)
        self.u.mem_write(a,struct.pack('<HH',x,y))
    def ret(self,v=0):
        self.u.reg_write(R.UC_ARM_REG_R0,v&0xFFFFFFFF);self.u.reg_write(R.UC_ARM_REG_PC,self.u.reg_read(R.UC_ARM_REG_LR))
    def call(self,where,*args,end=RETURN):
        u=self.u;where=self.sym[where] if isinstance(where,str) else where
        u.reg_write(R.UC_ARM_REG_SP,STACK+0xF000);u.reg_write(R.UC_ARM_REG_LR,RETURN|1)
        for i,v in enumerate(args):
            if i<4:u.reg_write(getattr(R,f'UC_ARM_REG_R{i}'),v)
            else:word(u,STACK+0xF000+4*(i-4),v)
        for i in range(4,12):u.reg_write(getattr(R,f'UC_ARM_REG_R{i}'),0x12340000+i)
        try:u.emu_start(where|1,end,count=250000)
        except Exception:
            print('ARM failure',hex(u.reg_read(R.UC_ARM_REG_PC)),self.logs[-5:]);raise
        assert u.reg_read(R.UC_ARM_REG_PC)==end,hex(u.reg_read(R.UC_ARM_REG_PC))
        assert u.reg_read(R.UC_ARM_REG_SP)==STACK+0xF000
        for i in range(4,12):assert u.reg_read(getattr(R,f'UC_ARM_REG_R{i}'))==0x12340000+i
        assert not any(self.mutex.values()),self.mutex
        return u.reg_read(R.UC_ARM_REG_R0)

    def branch_target(self,offset):
        a,b=struct.unpack_from('<HH',self.shell,offset);sign=(a>>10)&1
        imm=(sign<<24)|((1^((b>>13)&1)^sign)<<23)|((1^((b>>11)&1)^sign)<<22)|((a&1023)<<12)|((b&2047)<<1)
        if sign:imm-=1<<25
        pc=offset+4
        if not (b&0x1000):pc&=~3
        return SHELL+pc+imm

    def relocate_ready(self):
        # Apply only the firmware loader's local MOVW/MOVT address rebasing in
        # this small callback; branch immediates stay invariant under rebasing.
        old_base=0x83200DC0 if self.profile==2 else 0x81000000
        old_data=[0x81542000,0x81543000,0x83780FB0][self.profile]
        low={}
        for off in range(0x1BFE,0x1C48,2):
            a,b=struct.unpack_from('<HH',self.shell,off);op=a&0xFBF0
            if op not in (0xF240,0xF2C0) or b&0x8000:continue
            register=(b>>8)&15;value=((a&15)<<12)|((a&0x400)<<1)|((b&0x7000)>>4)|(b&255)
            if op==0xF240:low[register]=(off,value)
            elif register in low:
                start,small=low.pop(register);full=(value<<16)|small;dest=None
                if old_base<=full<old_base+len(self.shell):dest=SHELL+full-old_base
                elif old_data<=full<old_data+0xA0000:dest=SHELLDATA+full-old_data
                if dest is not None:self.mov(SHELL+start,0xF240,register,dest&65535);self.mov(SHELL+off,0xF2C0,register,dest>>16)
        self.get_plugin_interface=self.branch_target(0x1C02)
        self.ready_environment_check=self.branch_target(0x1C16)

    def module_interface(self,wrapper):
        impl=self.rd(wrapper);head=self.rd(impl+16);node=self.alloc(24)
        for off,v in [(0,head),(4,head),(8,head),(12,1),(16,RECDATA)]:word(self.u,node+off,v)
        for off in (0,4,8):word(self.u,head+off,node)
        word(self.u,impl+20,1)

    def native_initialize(self):
        cpu=rec.emulator(self.rectext)
        for off,_,_ in rec.PATCHES:cpu.mem_write(rec.BASE+off,bytes(self.u.mem_read(REC+off,4 if off not in (0x6084,) else 2)))
        visible,hidden=self.counts
        def hook(c,pc,size,_):
            rel=pc-rec.BASE;value=None
            if rel==0x4BF4:word(c,c.reg_read(R.UC_ARM_REG_R0),0);value=0
            elif rel in (0xC994,0xCCB6,0xCB36,0x2AF44,0x2AD54,0x2AE64,0x4DE,0x2BCE4,0x2B524):value=0
            elif rel==0x2AF24:value=hidden
            elif rel==0x2AD44:value=visible
            elif rel==0x2BC74:c.mem_write(c.reg_read(R.UC_ARM_REG_R0),bytes(16));value=0
            elif rel==0x2BC24:value=1
            elif rel==0x2B184:raise AssertionError('Native recovery stack guard')
            if value is not None:c.reg_write(R.UC_ARM_REG_R0,value);c.reg_write(R.UC_ARM_REG_PC,c.reg_read(R.UC_ARM_REG_LR))
        cpu.hook_add(UC_HOOK_CODE,hook);cpu.reg_write(R.UC_ARM_REG_R0,rec.STACK+0x100)
        cpu.emu_start(rec.BASE+0x9B,rec.RETURN,count=10000);assert cpu.reg_read(R.UC_ARM_REG_PC)==rec.RETURN
        flags=struct.unpack('<I',cpu.mem_read(rec.BASE+0x40100,4))[0]
        word(self.u,RECDATA+0x1100,flags);self.init_cpu=cpu;self.native_inits+=1;return flags

    def step(self,u,pc,size,_):
        args=[u.reg_read(getattr(R,f'UC_ARM_REG_R{i}')) for i in range(4)];name=self.names.get(pc);rel=pc-self.paf
        if name in ('debug_log_open','debug_log_close','debug_log_flush'):pass
        elif name=='debug_logf':self.logs.append(self.string(args[1]))
        elif name=='sceKernelCreateLwMutex':assert args[2:]==[0,0];self.mutex[args[0]]=0
        elif name=='sceKernelDeleteLwMutex':assert not self.mutex[args[0]]
        elif name=='sceKernelLockLwMutex':assert not self.mutex.get(args[0],0);self.mutex[args[0]]=1
        elif name=='sceKernelUnlockLwMutex':assert self.mutex[args[0]];self.mutex[args[0]]=0
        elif name=='taiGetModuleInfo':
            which=self.string(args[0])
            if which=='ScePaf':uid,nid=90,self.pnid
            else:
                assert which=='SceDbRecovery'
                if not self.available:self.ret(-1);return
                uid,nid=42,self.rnid
            word(u,args[1]+4,uid);word(u,args[1]+8,nid)
        elif name=='sceKernelGetModuleInfo':
            assert args[0] in (42,90)
            segments=[(self.paf,0x300D00),(self.pdata,0x166D0)] if args[0]==90 else [(REC,0x3E9E8),(RECDATA,0x3094)]
            for i,(ptr,length) in enumerate(segments):word(u,args[1]+0x15C+i*24,ptr);word(u,args[1]+0x160+i*24,length)
        elif name=='taiInjectDataForUser':
            a=[self.rd(args[0]+4*i) for i in range(7)];_,uid,segment,offset,_,source,length=a
            number=self.injections;self.injections+=1
            if number==self.fail_at:self.ret(-1);return
            assert uid in (7,42) and (uid!=7 or (segment==0 and offset==0xC1E))
            at=(SHELL if uid==7 else RECDATA if segment else REC)+offset
            self.patches[100+number]=(at,bytes(u.mem_read(at,length)));u.mem_write(at,bytes(u.mem_read(source,length)));self.ret(100+number);return
        elif name=='taiInjectRelease':
            if args[0]==self.fail_release:self.ret(-1);return
            at,previous=self.patches.pop(args[0]);u.mem_write(at,previous)
        elif name in ('sceClibMemcpy','memcpy'):
            u.mem_write(args[0],bytes(u.mem_read(args[1],args[2])));self.ret(args[0]);return
        elif name in ('sceClibMemset','memset'):
            u.mem_write(args[0],bytes([args[1]&255])*args[2]);self.ret(args[0]);return
        elif rel in (0x53D50,0x53D64):
            if rel==0x53D50:assert not self.mutex.get(args[0],0);self.mutex[args[0]]=1
            else:assert self.mutex[args[0]];self.mutex[args[0]]=0
        elif rel==0x536FC:self.ret(self.alloc(args[0]));return
        elif rel==0x53778:self.free(args[0])
        elif rel==0x51BC8:self.ret(len(self.string(args[0])));return
        elif rel in (0x1DC174,0x1DC1EC):
            data=bytes(u.mem_read(args[1],args[2])) if rel==0x1DC174 else self.string(self.rd(args[1])).encode()
            buf=self.alloc(len(data)+1);u.mem_write(buf,data+b'\0');word(u,args[0],buf);word(u,args[0]+4,len(data));word(u,args[0]+8,len(data));self.ret(args[0]);return
        elif rel==0x1E4538:word(u,args[0],self.cache.get(self.string(self.rd(args[2])),self.cachehead))
        elif rel==0x1E43FC:
            key=self.string(self.rd(args[1]));assert key not in self.cache;node=self.alloc(32);self.cache[key]=node;self.ret(node);return
        elif rel==0x1E495C:
            key=self.string(self.rd(args[1]));self.free(self.cache.pop(key))
        elif rel==0x1E4ABC:
            head=self.rd(args[0]);node=self.rd(head)
            if node!=head:self.free(node)
        elif rel==0x262578:u.reg_write(R.UC_ARM_REG_R1,0)
        elif rel==0x5076A:
            u.mem_write(args[0],bytes([args[1]&255])*args[2]);self.ret(args[0]);return
        elif rel==0x262588:
            assert self.string(args[0])=='vs0:vsh/common/dbrecovery_plugin.suprx' and args[1]==0
            assert self.rd(args[2])==4;self.available=True;self.ret(42);return
        elif rel==0x262478:
            assert args[0]==42 and args[1]==4 and args[3]==0
            assert not self.mutex.get(self.sym['lifecycle_mutex'],0)
            self.start_calls+=1;wrapper=self.rd(args[2]);
            if not self.omit_interface:self.module_interface(wrapper)
            word(u,self.rd(u.reg_read(R.UC_ARM_REG_SP)+4),0);self.ret(self.kernel_failure);return
        elif rel==0x262408:
            assert args[0]==42 and not any(self.mutex.values())
            self.stop_calls+=1;self.active_stop_return.append((u.reg_read(R.UC_ARM_REG_LR),self.rd(u.reg_read(R.UC_ARM_REG_SP)+4)))
            u.reg_write(R.UC_ARM_REG_R0,0);u.reg_write(R.UC_ARM_REG_R1,0);u.reg_write(R.UC_ARM_REG_LR,RETURN+0x401);u.reg_write(R.UC_ARM_REG_PC,REC+0x1F);return
        elif pc==RETURN+0x400:
            lr,out=self.active_stop_return.pop();word(u,out,args[0]);u.reg_write(R.UC_ARM_REG_R0,0 if args[0]==0 else 0x80000001);u.reg_write(R.UC_ARM_REG_PC,lr);return
        elif rel==0x262368:assert args[0]==42;self.available=False
        elif rel==0x516F0:pass
        elif pc==REC+0x9A:
            assert args[0]==PLUGIN;self.native_initialize()
        elif pc==REC+0x1E:
            # On first entry an installed literal redirect must execute. Once
            # restored, model native runtime finalization (the immutable body
            # is checked by the redirect test and returns SCE_STOP_SUCCESS).
            if self.u.mem_read(pc,2)==b'\xdf\xf8':return
            assert bytes(u.mem_read(pc,10))==self.rectext[0x1E:0x28]
        elif pc==SHELL+self.import_load:
            assert args[0]==PARAM and args[2]==0;self.callbacks.append(args[1])
        elif pc==self.get_plugin_interface:
            self.ret(RECDATA+0x18 if args[0] else 0);return
        elif pc==self.ready_environment_check:self.ret(1);return
        elif pc==SHELL+0x1BFE:
            assert args[0] in (PLUGIN,0);self.finished+=1;return
        elif pc==REC+0x21C:
            assert args[0]==SHELL+0x1BE3
            # Completion callback registration uses the actual native recovery
            # entry in the separate firmware context; it must not change flags.
            if self.init_cpu:
                c=self.init_cpu;c.reg_write(R.UC_ARM_REG_SP,rec.STACK+0xF000);c.reg_write(R.UC_ARM_REG_LR,rec.RETURN|1);c.reg_write(R.UC_ARM_REG_R0,SHELL+0x1BE3)
                c.emu_start(rec.BASE+0x21D,rec.RETURN,count=1000);assert c.reg_read(R.UC_ARM_REG_PC)==rec.RETURN
                flags=struct.unpack('<I',c.mem_read(rec.BASE+0x40100,4))[0];assert flags==self.rd(RECDATA+0x1100)
        else:return
        self.ret()

    def execute(self):
        assert self.call('recovery_start',7,self.nid,INFO)==0
        assert self.call('recovery_stop')==0xFFFFFFFF
        self.call(SHELL+0xC1E,PARAM,SHELL+0x1BFF,0,end=SHELL+0xC22)
        assert self.available and self.start_calls==1 and len(self.callbacks)==1
        held=self.rd(self.sym['preload']);assert self.rd(held+28)==1
        assert self.call(self.paf+0x4722A,OWNER,FILENAME,0,1,0)==OWNER
        assert self.rd(OWNER)==held and self.rd(held+28)==2 and self.start_calls==1
        word(self.u,PLUGIN+48,OWNER)
        # Execute the actual native Plugin constructor's interface-copy block.
        u=self.u;u.reg_write(R.UC_ARM_REG_R6,PLUGIN);u.reg_write(R.UC_ARM_REG_R8,PARAM)
        u.emu_start(self.paf+0x588DB,self.paf+0x58904,count=1000)
        assert self.rd(PLUGIN+32)==self.rd(RECDATA+4)
        u.reg_write(R.UC_ARM_REG_SP,STACK+0xF000);u.reg_write(R.UC_ARM_REG_R6,PLUGIN)
        u.emu_start(self.paf+0x58E5F,self.paf+0x58E6A,count=200000)
        assert self.native_inits==1 and self.rd(RECDATA+0x1100)==self.expected_flags
        self.call(self.callbacks.pop(),PLUGIN)
        assert self.finished==1 and self.rd(held+28)==1 and self.rd(self.sym['preload'])==0
        self.call(self.paf+0x473D4,OWNER)
        assert self.stop_calls==1 and not self.available and not self.cache
        assert len(self.patches)==1 and len(self.live)==2  # Permanent call site; fixture heap only.
        for off,expected,_ in rec.PATCHES:assert bytes(u.mem_read(REC+off,len(expected)))==expected
        for i,offset in enumerate(self.offsets):assert self.rd(RECDATA+i*4)==REC+offset
        assert self.call('recovery_stop')==0xFFFFFFFF


def main():
    p=argparse.ArgumentParser();p.add_argument('plugin',type=Path);p.add_argument('evidence_root',type=Path);p.add_argument('--database',action='append',type=Path,default=[]);a=p.parse_args()
    cases=[(500,49),(500,73)]
    for db in a.database:
        c=sqlite3.connect('file:'+str(db)+'?mode=ro',uri=True)
        hidden=c.execute('SELECT COUNT(*) FROM tbl_appinfo_icon i JOIN tbl_appinfo_page p USING(pageId) WHERE type=0 AND pageNo=-100000000').fetchone()[0]
        visible=c.execute('SELECT COUNT(*) FROM tbl_appinfo_icon i JOIN tbl_appinfo_page p USING(pageId) WHERE type=0 AND pageNo<>-100000000').fetchone()[0]
        if (visible,hidden) not in cases:cases.append((visible,hidden))
    binary=elf(a.plugin)
    print("Thumb BL call encoder:",check_encoder(binary,link=True),"boundary/invalid cases passed")
    for firmware in range(3):
        for counts in cases:
            h=Trial(binary,a.evidence_root,firmware,counts);h.execute();print('PASS profile',firmware,'counts',counts,'native Module cache reuse, pre-init flag, completion, rollback')
    fault_cases=0
    for firmware in range(3):
        for point in ((1,4,9) if 'recovery_init_observer' in binary[2] else (1,4)):
            h=Trial(binary,a.evidence_root,firmware,(500,49));h.fail_at=point;h.expected_flags=0;h.execute();fault_cases+=1
        for failure in ('start','interface','partial','load-failed','stop-cleanup'):
            h=Trial(binary,a.evidence_root,firmware,(500,49))
            if failure=='start':h.kernel_failure=-1
            if failure=='interface':h.omit_interface=True
            if failure=='partial':h.fail_at=4;h.fail_release=102
            assert h.call('recovery_start',7,h.nid,INFO)==0
            h.call(SHELL+0xC1E,PARAM,SHELL+0x1BFF,0,end=SHELL+0xC22)
            if failure in ('start','interface'):
                assert h.rd(h.sym['preload'])==0 and not h.cache and h.callbacks==[SHELL+0x1BFF]
            elif failure=='partial':
                assert not h.callbacks and h.rd(h.sym['preload']) and not h.native_inits
            elif failure=='stop-cleanup':
                h.fail_release=102;h.call(h.callbacks.pop(),0)
                assert h.available and not h.cache and h.rd(h.sym['preload'])==0 and h.rd(h.sym['preload_phase'])==4
                h.call(SHELL+0xC1E,PARAM,SHELL+0x1BFF,0,end=SHELL+0xC22);assert not h.callbacks
            else:
                h.call(h.callbacks.pop(),0)
                assert not h.available and not h.cache and h.rd(h.sym['preload'])==0 and len(h.live)==2
            fault_cases+=1
    print('Compiled ARM recovery preload:',len(cases)*3+fault_cases,'scenarios passed. Native PAF Module code, recovery initializer/decision and original SceShell ready callback run; OS, maps/strings, scheduling and UI are modeled.')
if __name__=='__main__':main()
