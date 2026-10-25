"""Managed SDK vision entry; experimental firmware is a separate source reference."""
import asyncio,json
from pathlib import Path
import uvicorn
from watcherobot.application import ApplicationContext
from configuration import validate_config
from download_models import ensure_models
from service import VisionService,create_web_app

async def main():
 root=Path(__file__).resolve().parent
 config=validate_config(json.loads((root/'config.json').read_text(encoding='utf-8')))
 async with ApplicationContext.from_environment() as app:
  app.logger.info('Preparing pinned public Google models. Images and audio are not uploaded.')
  await asyncio.to_thread(ensure_models,root/'models')
  service=VisionService(app.robot,app.logger,root,config)
  server=uvicorn.Server(uvicorn.Config(create_web_app(service),host='127.0.0.1',port=config['port'],log_level='warning',access_log=False,timeout_graceful_shutdown=5))
  task=None
  try:
   service.start();task=asyncio.create_task(server.serve())
   app.logger.info('Tangyuan SDK preview: http://127.0.0.1:%s ; initially paused.',config['port'])
   while not app.shutdown_requested and not task.done():await asyncio.sleep(.15)
   if task.done():task.result()
  finally:
   service.request_shutdown();server.should_exit=True
   try:
    if task is not None:await task
   finally:await asyncio.to_thread(service.close)
if __name__=='__main__':asyncio.run(main())
