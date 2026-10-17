import io,json,time,os,statistics,hashlib,cProfile,pstats
from pathlib import Path
from voice_bridge.worker import WhisperTranscriber
from voice_bridge.render import RenderPages
from voice_bridge.assets import font_path
out=Path('/tmp/my-bot-latency')
audio=Path('/tmp/my-bot-auto-reply-test.wav').read_bytes()
r={'input_sha256':hashlib.sha256(audio).hexdigest(),'bytes':len(audio),'duration_s':(len(audio)-44)/96000,'load_before':os.getloadavg(),'runs':[]}
t=time.perf_counter(); tr=WhisperTranscriber(); model=tr.load(); r['load_s']=time.perf_counter()-t
variants=[('current',{}),('zh',{'language':'zh'}),('zh_beam1',{'language':'zh','beam_size':1,'best_of':1})]
def run(opts):
 t=time.perf_counter(); segments,info=model.transcribe(io.BytesIO(audio),vad_filter=True,**opts); text=''.join(s.text for s in segments).strip(); elapsed=time.perf_counter()-t
 return {'seconds':elapsed,'text':text,'language':info.language}
r['first_current']=run({})
for i in range(5):
 for name,opts in variants:
  item={'round':i+1,'variant':name,**run(opts)};r['runs'].append(item); print(json.dumps(item,ensure_ascii=False),flush=True)
prof=cProfile.Profile();prof.enable();run({});prof.disable();prof.dump_stats(str(out/'asr-profile.prof'))
with (out/'asr-profile.txt').open('w') as f:pstats.Stats(prof,stream=f).sort_stats('cumulative').print_stats(25)
t=time.perf_counter();render=RenderPages(font_path());r['render_init_s']=time.perf_counter()-t
r['render']=[]
for i in range(5):
 t=time.perf_counter(); pages=render('自动回复已接通。');elapsed=time.perf_counter()-t
 r['render'].append({'seconds':elapsed,'pages':len(pages.pages),'bytes':sum(map(len,pages.pages)),'sha256':hashlib.sha256(b''.join(pages.pages)).hexdigest()})
r['load_after']=os.getloadavg()
for name,_ in variants:
 v=[x['seconds'] for x in r['runs'] if x['variant']==name];r[name]={'median_s':statistics.median(v),'min_s':min(v),'max_s':max(v)}
(out/'results.json').write_text(json.dumps(r,ensure_ascii=False,indent=2))
print(json.dumps({k:v for k,v in r.items() if k not in ('runs','render')},ensure_ascii=False),flush=True)
