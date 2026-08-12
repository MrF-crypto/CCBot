import io,sys,statistics as st

def hourly_ohlc(sym, years):
    """流式聚合 1m -> 1h OHLC，不把全部分钟数据留在内存"""
    out=[]; cur=None; o=h=l=c=0.0
    for y in years:
        try: f=io.open(f'{y}/{sym}-USDT.csv',encoding='gbk',errors='replace')
        except OSError: continue
        with f:
            hdr=None
            for line in f:
                p=line.rstrip('\n').split(',')
                if hdr is None:
                    lw=[x.strip().lower() for x in p]
                    if 'close' in lw:
                        hdr={k:lw.index(k) for k in ('candle_begin_time','open','high','low','close')}
                    continue
                try:
                    t=p[hdr['candle_begin_time']][:13]
                    O=float(p[hdr['open']]); H=float(p[hdr['high']])
                    L=float(p[hdr['low']]);  C=float(p[hdr['close']])
                except (ValueError,IndexError): continue
                if t!=cur:
                    if cur is not None: out.append((cur,o,h,l,c))
                    cur=t; o,h,l,c=O,H,L,C
                else:
                    h=max(h,H); l=min(l,L); c=C
    if cur is not None: out.append((cur,o,h,l,c))
    return out

def rsi_series(closes, period=14):
    """与 indicators.h 完全一致的 Wilder RSI，逐点输出"""
    n=len(closes); out=[None]*n
    if n<period+1: return out
    g=l=0.0
    for i in range(1,period+1):
        d=closes[i]-closes[i-1]
        if d>0: g+=d
        else:   l-=d
    g/=period; l/=period
    out[period]=100.0 if l<1e-10 else 100.0-100.0/(1.0+g/l)
    for i in range(period+1,n):
        d=closes[i]-closes[i-1]
        if d>0: g=(g*(period-1)+d)/period; l=l*(period-1)/period
        else:   g=g*(period-1)/period;     l=(l*(period-1)-d)/period
        out[i]=100.0 if l<1e-10 else 100.0-100.0/(1.0+g/l)
    return out

BUCKETS=[(0,10),(10,15),(15,20),(20,25),(25,30),(30,35),(35,40),(40,50),(50,60),(60,100)]
HOR=168          # 前瞻窗口(小时)=7天
BOT_W=48         # 局部底判定窗口 ±48h

def analyse(bars, label):
    closes=[b[4] for b in bars]; highs=[b[2] for b in bars]; lows=[b[3] for b in bars]
    r=rsi_series(closes)
    n=len(bars)
    acc={b:{'n':0,'f24':[],'f72':[],'f168':[],'mae':[],'mfe':[],'bot':0} for b in BUCKETS}
    for i in range(len(closes)):
        if r[i] is None or i+HOR>=n or i<BOT_W: continue
        v=r[i]
        for b in BUCKETS:
            if b[0]<=v<b[1]:
                a=acc[b]; a['n']+=1
                c0=closes[i]
                a['f24'].append(closes[i+24]/c0-1)
                a['f72'].append(closes[i+72]/c0-1)
                a['f168'].append(closes[i+HOR]/c0-1)
                a['mae'].append(min(lows[i+1:i+HOR+1])/c0-1)
                a['mfe'].append(max(highs[i+1:i+HOR+1])/c0-1)
                # 局部底：当前收盘是前后各48h内的最低收盘
                if c0<=min(closes[max(0,i-BOT_W):i+BOT_W+1]): a['bot']+=1
                break
    print(f"\n===== {label}  （{n} 根1h，前瞻{HOR}h/7天）=====")
    print(f"{'RSI区间':<10}{'样本':>8}{'占比%':>7}{'+24h%':>8}{'+72h%':>8}{'+7d%':>8}"
          f"{'7d胜率%':>9}{'平均MAE%':>10}{'平均MFE%':>10}{'是局部底%':>11}")
    tot=sum(acc[b]['n'] for b in BUCKETS) or 1
    for b in BUCKETS:
        a=acc[b]
        if a['n']<30: continue
        wr=sum(1 for x in a['f168'] if x>0)/len(a['f168'])*100
        print(f"{b[0]:>3}-{b[1]:<6}{a['n']:>8}{a['n']/tot*100:>7.1f}"
              f"{st.mean(a['f24'])*100:>8.2f}{st.mean(a['f72'])*100:>8.2f}"
              f"{st.mean(a['f168'])*100:>8.2f}{wr:>9.1f}"
              f"{st.mean(a['mae'])*100:>10.2f}{st.mean(a['mfe'])*100:>10.2f}"
              f"{a['bot']/a['n']*100:>11.1f}")

if __name__=='__main__':
    sym=sys.argv[1]; years=sys.argv[2].split(',')
    bars=hourly_ohlc(sym,years)
    analyse(bars,f"{sym}  {years[0]}~{years[-1]}")
