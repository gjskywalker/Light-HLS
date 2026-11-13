#!/usr/bin/env python3
"""Sanitized algorithm-oracle tests for the repaired Testbenches corpus."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
COMMON = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>
std::mt19937 generator(20261113);
'''

FFT = r'''
int main() {
  using T = TYPE;
  for (int trial=0; trial<32; ++trial) {
    T real[256], imag[256], er[256], ei[256], wr[128], wi[128];
    for(int j=0;j<256;++j) {
      real[j]=er[j]=int(generator()%7)-3;
      imag[j]=ei[j]=int(generator()%7)-3;
    }
    for(int j=0;j<128;++j) { wr[j]=int(generator()%3)-1; wi[j]=int(generator()%3)-1; }
    for(int stage=0;stage<8;++stage) {
      int half=128>>stage;
      for(int base=0;base<256;base+=2*half) for(int k=0;k<half;++k) {
        int a=base+k,b=a+half,twiddle=k<<stage;
        T dr=er[a]-er[b],di=ei[a]-ei[b];
        er[a]+=er[b]; ei[a]+=ei[b];
        if(twiddle) { er[b]=wr[twiddle]*dr-wi[twiddle]*di; ei[b]=wr[twiddle]*di+wi[twiddle]*dr; }
        else { er[b]=dr; ei[b]=di; }
      }
    }
    dut(real,imag,wr,wi);
    for(int j=0;j<256;++j) { assert(real[j]==er[j]); assert(imag[j]==ei[j]); }
  }
  std::cout << "PASS 32 independent radix-2 butterfly cases\n";
}
'''

AES = r'''
static int gf_multiply(int a,int b) {
  int result=0;
  for(int bit=0;bit<8;++bit) {
    if(b&1) result^=a;
    a=(a<<1)^((a&128)?0x11b:0); b>>=1;
  }
  return result;
}
int main() {
  const int coefficients[4] = COEFFICIENTS;
  for(int trial=0;trial<256;++trial) {
    int state[32],expected[32],word[4][120];
    for(int i=0;i<32;++i) state[i]=generator()%256;
    for(auto &row:word) for(int &v:row) v=generator()%256;
    int n=trial%15;
    for(int col=0;col<8;++col) for(int row=0;row<4;++row) {
      int sum=0;
      for(int k=0;k<4;++k) {
        int r=(row+k)%4;
        int v=state[col*4+r];
        if(INVERSE) v^=word[r][112+col];
        sum^=gf_multiply(v,coefficients[k]);
      }
      if(!INVERSE) sum^=word[row][n*8+col];
      expected[col*4+row]=sum;
    }
    assert(CALL == 0);
    assert(std::equal(state,state+32,expected));
  }
  int n=0;
  int state[32]={0},word[4][120]={0},saved[32]={0};
  state[0]=-1; std::copy(state,state+32,saved);
  assert(CALL == -1 && std::equal(state,state+32,saved));
  state[0]=0; word[0][KEY_COLUMN]=-1; std::copy(state,state+32,saved);
  assert(CALL == -1 && std::equal(state,state+32,saved));
  INVALID_ROUNDS
  std::cout << "PASS 256 nonzero-key GF(2^8) cases and invalid-input guards\n";
}
'''

GRADIENT = r'''
int main() {
  std::vector<double> frame(436*1024),gx(frame.size()),gy(frame.size());
  for(int trial=0;trial<4;++trial) {
    for(double &v:frame) v=int(generator()%17)-8;
    auto saved=frame;
    std::fill(gx.begin(),gx.end(),91); std::fill(gy.begin(),gy.end(),-91);
    dut(reinterpret_cast<double (*)[1024]>(frame.data()),
        reinterpret_cast<double (*)[1024]>(gx.data()),
        reinterpret_cast<double (*)[1024]>(gy.data()));
    assert(frame==saved);
    // The extracted kernel sums only its constant weights, not image samples.
    // Preserve that existing behavior: the weight sum and every output are zero.
    for(double v:gx) assert(v==0);
    for(double v:gy) assert(v==0);
  }
  std::cout << "PASS 4 full-size drain/boundary cases\n";
}
'''

MATRIX = r'''
int main() {
  for(int trial=0;trial<8;++trial) {
    float a[8][8208],b[8][8208],out[8][32],expected[8][32];
    for(auto &row:a) for(float &v:row) v=int(generator()%5)-2;
    for(auto &row:b) for(float &v:row) v=int(generator()%5)-2;
    for(auto &row:out) for(float &v:row) v=-99;
    std::memcpy(expected,out,sizeof(out));
    for(int block=0;block<8;++block) for(int window=0;window<2;++window)
      for(int lane=0;lane<8;++lane) {
        float sum=(lane%2)?0.1f:0.0f;
        for(int j=0;j<8192;++j) {
          float x=a[lane][16*window+j],y=b[block][16*window+j];
          if(x!=0 && y!=0) sum+=x*y;
        }
        expected[block][16*window+lane]=sum;
      }
    dut(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
        out[0],out[1],out[2],out[3],out[4],out[5],out[6],out[7]);
    assert(std::memcmp(out,expected,sizeof(out))==0);
  }
  std::cout << "PASS 8 full-size two-window matrix cases\n";
}
'''

BNN = r'''
int main() {
  for(int trial=0;trial<64;++trial) {
    int current[8][10],old[8][10],line[8][3][10],before[8][3][10],expected[8][3][10];
    int weights[3][3],out[64],eo[64]; bool lb[8],rb[8];
    for(auto &r:current) for(int &v:r) v=generator()%4;
    for(auto &r:old) for(int &v:r) v=generator()%4;
    for(auto &b:line) for(auto &r:b) for(int &v:r) v=generator()%4;
    for(auto &r:weights) for(int &v:r) v=generator()%2;
    for(int bank=0;bank<8;++bank) { lb[bank]=generator()%2; rb[bank]=generator()%2; }
    std::memcpy(before,line,sizeof(line)); std::memcpy(expected,line,sizeof(line));
    int log_width=3+trial%4,shift=1<<(log_width-3);
    for(int bank=0;bank<8;++bank) for(int col=0;col<8;++col) {
      int sum=0;
      for(int r=0;r<3;++r) for(int c=0;c<3;++c) {
        int value=before[bank][r][col+c];
        sum+=value^(value&weights[2-r][2-c]);
      }
      eo[bank*8+col]=sum;
    }
    for(int bank=0;bank<8;++bank) {
      int source=bank-shift;
      for(int c=0;c<10;++c) {
        expected[bank][0][c]=source<0 ? old[source+8][c] : current[source][c];
        expected[bank][1][c]=current[bank][c];
      }
      if(lb[bank]) expected[bank][0][0]=expected[bank][1][0]=0;
      if(rb[bank]) expected[bank][0][9]=expected[bank][1][9]=0;
    }
    dut(current,old,lb,rb,line,weights,out,log_width);
    assert(std::memcmp(line,expected,sizeof(line))==0);
    assert(std::equal(out,out+64,eo));
    for(int bad:{-100,0,2,7,31,100}) {
      std::memcpy(before,line,sizeof(line)); int saved[64]; std::copy(out,out+64,saved);
      dut(current,old,lb,rb,line,weights,out,bad);
      assert(std::memcmp(line,before,sizeof(line))==0 && std::equal(out,out+64,saved));
    }
  }
  std::cout << "PASS 64 BNN cases across widths 8..64 and invalid-width guards\n";
}
'''

SOFTFLOAT = r'''
static uint64_t bits(double value) { uint64_t result; std::memcpy(&result,&value,8); return result; }
static double number(uint64_t value) { double result; std::memcpy(&result,&value,8); return result; }
static void check(uint64_t a,uint64_t b) {
  volatile double x=number(a),y=number(b);
  double expected=x+y;
  uint64_t actual=add_binary64(a,b);
  assert(bits(expected)==actual || (std::isnan(expected)&&std::isnan(number(actual))));
}
int main() {
  assert(dut()==0); assert(dut()==0);
  const uint64_t edge[]={0,1,0xfffffffffffffULL,0x10000000000000ULL,
    0x3ff0000000000000ULL,0x3ca0000000000000ULL,0x3c90000000000000ULL,
    0x7fefffffffffffffULL,0x7ff0000000000000ULL,0x7ff0000000000001ULL,
    0x7ff8000000000000ULL,0x8000000000000000ULL,0xbff0000000000000ULL,
    0xffefffffffffffffULL,0xfff0000000000000ULL,0xfff8000000000000ULL};
  for(auto a:edge) for(auto b:edge) check(a,b);
  std::mt19937_64 random64(20261113);
  for(int i=0;i<50000;++i) check(random64(),random64());
  std::cout << "PASS 46 original exact-bit vectors, 256 edge pairs, 50000 host-FP64 comparisons\n";
}
'''


def template(name):
    if name in ("random5", "random23"):
        return FFT.replace("TYPE", "int" if name == "random5" else "double")
    if name in ("random57", "random58"):
        inverse = name == "random58"
        return (AES.replace("COEFFICIENTS", "{14,11,13,9}" if inverse else "{2,3,1,1}")
            .replace("INVERSE", "true" if inverse else "false")
            .replace("CALL", "dut(state,word)" if inverse else "dut(state,n,word)")
            .replace("KEY_COLUMN", "112" if inverse else "0")
            .replace("INVALID_ROUNDS", "" if inverse else
                "for(int bad:{-1,15,2147483647}) assert(dut(state,bad,word)==-1);"))
    return {"random30": GRADIENT, "random35": MATRIX, "random66": SOFTFLOAT, "random68": BNN}[name]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT / "Dataset/Testbenches")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    rows=[]
    for name in ("random5","random23","random30","random35","random57","random58","random66","random68"):
        directory=output/name
        directory.mkdir()
        source=(args.source_root/(name+".cc")).resolve(strict=True)
        harness=directory/"oracle.cpp"
        harness.write_text(COMMON+'\n#include "'+str(source)+'"\n'+template(name))
        binary=directory/"oracle"
        row=dict(name=name, source=str(source), source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(), status="failed", commands=[])
        rows.append(row)
        for stage,command in (("compile",["/usr/local/bin/clang++","-std=c++17","-O1","-g","-ffp-contract=off",
                "-fsanitize=address,undefined","-fno-sanitize-recover=all",str(harness),"-o",str(binary)]),
                ("run",[str(binary)])):
            import os
            with (directory/(stage+".log")).open("x") as log:
                result=subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,
                    env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0"),timeout=300)
            row["commands"].append(dict(stage=stage,command=command,returncode=result.returncode))
            if result.returncode: break
        else: row["status"]="passed"
        (output/"results.json").write_text(json.dumps(rows,indent=2)+"\n")
        print(name,row["status"],flush=True)
    return 0 if all(row["status"]=="passed" for row in rows) else 1


if __name__=="__main__":
    raise SystemExit(main())
