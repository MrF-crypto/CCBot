import io,sys,statistics as st
sys.path.insert(0,'/tmp')
from rsi_study import hourly_ohlc, rsi_series

HOR=168
def cross_signals(bars, lo_th, hi_th):
    """框架实际用的信号：RSI 先跌破 lo_th（探底），再回穿 hi_th（确认反转）"""
    closes=[b[4] for b in bars]; highs=[b[2] for b in bars]; lows=[b[3] for b in bars]
    r=rsi_series(closes); n=len(closes)
    dipped=False; sig=[]
    for i in range(n):
        if r[i] is None: continue
        if r[i] < lo_th: dipped=True
        elif dipped and r[i] >= hi_th:
            sig.append(i); dipped=False
    out=[]
    for i in sig:
        if i+HOR>=n: continue
        c0=closes[i]
        out.append((closes[i+24]/c0-1, closes[i+72]/c0-1, closes[i+HOR]/c0-1,
                    min(lows[i+1:i+HOR+1])/c0-1, max(highs[i+1:i+HOR+1])/c0-1))
    return out

def run(bars,label):
    print(f"\n===== 交叉信号  {label} =====")
    print(f"{'探底<':>6}{'回穿>':>7}{'信号数':>8}{'+24h%':>8}{'+72h%':>8}{'+7d%':>8}"
          f"{'7d胜率%':>9}{'平均MAE%':>10}{'MFE/MAE':>9}")
    rows=[]
    for lo in (15,20,25,30):
        for hi in (25,30,35,40,45):
            if hi<=lo: continue
            s=cross_signals(bars,lo,hi)
            if len(s)<25: continue
            f7=[x[2] for x in s]; mae=[x[3] for x in s]; mfe=[x[4] for x in s]
            wr=sum(1 for x in f7 if x>0)/len(f7)*100
            ratio=abs(st.mean(mfe)/st.mean(mae)) if st.mean(mae)!=0 else 0
            rows.append((st.mean(f7),lo,hi,len(s),st.mean([x[0] for x in s]),
                         st.mean([x[1] for x in s]),st.mean(f7),wr,st.mean(mae),ratio))
    for _,lo,hi,n,f24,f72,f7,wr,mae,ratio in sorted(rows,reverse=True):
        print(f"{lo:>6}{hi:>7}{n:>8}{f24*100:>8.2f}{f72*100:>8.2f}{f7*100:>8.2f}"
              f"{wr:>9.1f}{mae*100:>10.2f}{ratio:>9.2f}")

if __name__=='__main__':
    for sym,yrs,lab in [('BTC',['2021','2022','2023','2024'],'BTC 2021-2024'),
                        ('BTC',['2025','2026'],'BTC 2025-2026')]:
        run(hourly_ohlc(sym,yrs), lab)
