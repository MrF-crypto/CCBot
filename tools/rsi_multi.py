import sys,statistics as st
sys.path.insert(0,'/tmp')
from rsi_study import hourly_ohlc, rsi_series
from rsi_cross import cross_signals
SYMS=['BTC','ETH','BNB','XRP','SOL','TRX','DOGE','ZEC','LINK','XLM']
for yrs,lab in [(['2021','2022','2023','2024'],'10品种 2021-2024'),
                (['2025','2026'],'10品种 2025-2026')]:
    pool={}
    for s in SYMS:
        b=hourly_ohlc(s,yrs)
        if len(b)<200: continue
        for lo in (15,20,25,30):
            for hi in (25,30,35,40,45):
                if hi<=lo: continue
                pool.setdefault((lo,hi),[]).extend(cross_signals(b,lo,hi))
    print(f"\n===== 交叉信号 {lab} =====")
    print(f"{'探底<':>6}{'回穿>':>7}{'信号数':>8}{'+7d%':>8}{'7d胜率%':>9}{'平均MAE%':>10}{'MFE/MAE':>9}")
    rows=[]
    for (lo,hi),s in pool.items():
        if len(s)<60: continue
        f7=[x[2] for x in s]; mae=[x[3] for x in s]; mfe=[x[4] for x in s]
        wr=sum(1 for x in f7 if x>0)/len(f7)*100
        rows.append((st.mean(f7),lo,hi,len(s),wr,st.mean(mae),
                     abs(st.mean(mfe)/st.mean(mae)) if st.mean(mae) else 0))
    for f7,lo,hi,n,wr,mae,ratio in sorted(rows,reverse=True):
        print(f"{lo:>6}{hi:>7}{n:>8}{f7*100:>8.2f}{wr:>9.1f}{mae*100:>10.2f}{ratio:>9.2f}")
