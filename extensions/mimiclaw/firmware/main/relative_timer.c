/* Local timing grammar. No model, network or web page. */
#include "relative_timer.h"
#include <string.h>
#include <stdio.h>
static int take(const char **p,const char *s){size_t n=strlen(s);if(strncmp(*p,s,n))return 0;*p+=n;return 1;}
static void spaces(const char **p){while(**p==' '||**p=='\t')(*p)++;}
static int digit(const char **p){
 const char *names[]={"零","一","二","三","四","五","六","七","八","九","两","〇"};
 if(**p>='0'&&**p<='9')return *(*p)++-'0';
 for(int i=0;i<12;i++)if(take(p,names[i]))return i==10?2:i==11?0:i;
 return -1;
}
static int number(const char **p){
 unsigned total=0,part=0,count=0;const char *begin=*p;
 for(;;){int d=digit(p);if(d>=0){part=part*10+(unsigned)d;count++;if(count>6)return -1;continue;}
  if(take(p,"十")){total+=(part?part:1)*10;part=0;count++;}
  else if(take(p,"百")){total+=(part?part:1)*100;part=0;count++;}
  else if(take(p,"千")){total+=(part?part:1)*1000;part=0;count++;}
  else break;
 }return begin==*p?-1:(int)(total+part);
}
static int verb(const char **p){return take(p,"提醒一下我")||take(p,"提醒我")||take(p,"提醒")||take(p,"叫醒我")||take(p,"叫我")||take(p,"喊我");}
static int empty(const char *p){while(*p){if(*p==' '||*p=='.'||*p=='!'||*p=='?'){p++;continue;}if(take(&p,"。")||take(&p,"！")||take(&p,"？")||take(&p,"吧")||take(&p,"一下"))continue;return 0;}return 1;}
static int message(const char *p,char *out,size_t n){
 spaces(&p);while(take(&p,"，")||take(&p,","))spaces(&p);
 const char *v=p;if(verb(&v))p=v;
 if(empty(p))p="时间到了。";
 else if(!strcmp(p,"起床")||!strcmp(p,"起床。"))p="该起床了。";
 else if(!strcmp(p,"准备开会")||!strcmp(p,"准备开会。"))p="准备开会。";
 if(strlen(p)>=n)return -1;
 snprintf(out,n,"%s",p);return 1;
}
static int duration(const char **p){
 int64_t sum=0;unsigned count=0;
 for(;;){spaces(p);const char *begin=*p;int n=number(p),half=0;if(n<0){if(take(p,"半")){n=0;half=1;}else break;}
  (void)take(p,"个");int before_half=take(p,"半");int unit=take(p,"小时")?3600:take(p,"分钟")?60:take(p,"分")?60:take(p,"秒钟")?1:take(p,"秒")?1:take(p,"天")?86400:0;
  if(!unit){*p=begin;break;}int after_half=take(p,"半");sum+=(int64_t)n*unit;if(half||before_half||after_half)sum+=unit/2;
  count++;if(sum>604800||count>4)return -1;
 }return count?(sum?(int)sum:-1):0;
}
int ty_parse_voice_schedule(const char *text,time_t now,ty_voice_schedule_t *out){
 if(!text||!out||strlen(text)>900)return 0;
 memset(out,0,sizeof(*out));const char *p=text;
 spaces(&p);for(int i=0;i<3;i++){if(!(take(&p,"请你")||take(&p,"请")||take(&p,"帮我")||take(&p,"给我")))break;spaces(&p);}
 int leading=verb(&p);spaces(&p);int every=take(&p,"每隔")||take(&p,"每");const char *duration_start=p;int seconds=duration(&p);
 if(seconds){
  if(seconds<0)return -1;
  if(!every&&!(take(&p,"以后")||take(&p,"之后")||take(&p,"后")))return 0;
  spaces(&p);int has_verb=verb(&p);if(!leading&&!has_verb)return 0;
  if(seconds<(every?60:5))return -1;
  out->seconds=(unsigned)seconds;out->repeating=every!=0;return message(p,out->message,sizeof(out->message));
 }
 if(every)return 0;
 p=duration_start;int day=-1,period=0;
 if(take(&p,"明早")){day=1;period=1;}else if(take(&p,"明天"))day=1;else if(take(&p,"后天"))day=2;else if(take(&p,"今天"))day=0;
 else if(take(&p,"今晚")){day=0;period=2;}
 if(take(&p,"早上")||take(&p,"上午")||take(&p,"凌晨"))period=1;
 else if(take(&p,"下午")||take(&p,"晚上")||take(&p,"傍晚"))period=2;
 else if(take(&p,"中午"))period=3;
 int hour=number(&p);if(hour<0||!(take(&p,"点")||take(&p,"时")))return 0;
 int minute=0;if(take(&p,"半"))minute=30;else if(take(&p,"一刻"))minute=15;else if(take(&p,"三刻"))minute=45;
 else{const char *before=p;int m=number(&p);if(m>=0){minute=m;(void)take(&p,"分钟");(void)take(&p,"分");}else p=before;}
 spaces(&p);if(!leading&&!verb(&p))return 0;
 if(now<1735689600||hour>23||minute>59)return -1;
 if(period==1&&hour==12)hour=0;else if(period==2&&hour<12)hour+=12;else if(period==3&&hour>=1&&hour<=10)hour+=12;
 time_t local=now+28800;struct tm tm;gmtime_r(&local,&tm);int64_t midnight=(int64_t)now-(tm.tm_hour*3600+tm.tm_min*60+tm.tm_sec);
 int64_t at=midnight+(day<0?0:day)*86400+hour*3600+minute*60;
 if(day<0&&at<=now)at+=86400;
 if(at<=now||at>now+366LL*86400)return -1;
 out->at_epoch=at;return message(p,out->message,sizeof(out->message));
}
int ty_parse_relative_timer(const char *text,unsigned *seconds,char *message_out,size_t capacity){
 ty_voice_schedule_t r;int ok=ty_parse_voice_schedule(text,time(NULL),&r);if(ok<=0)return ok;if(r.at_epoch||r.repeating)return 0;
 if(strlen(r.message)>=capacity)return -1;
 *seconds=r.seconds;snprintf(message_out,capacity,"%s",r.message);return 1;
}
