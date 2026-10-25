/* Explicit public commands, not global ASR word replacement. */
#include "interaction_intent.h"
#include <string.h>
static bool has(const char *s,const char *q){return strstr(s,q)!=0;}
ty_hri_intent_t ty_hri_intent(const char *s,bool pending){
 if(!s)return TY_HRI_NONE;
 const char *busy[]={"我在忙","我还没空","我还没有空","我没空","晚点告诉我","先别提醒","先别汇报","不要汇报","暂时不汇报","先不汇报","不用汇报","晚点再说","稍后再汇报","别播报","不要播报","晚点再汇报","不是让你汇报","不是要汇报","不是说我空了","不要把结果告诉我","先别告诉我","不用告诉我","我不想听结果"};
 for(unsigned i=0;i<sizeof(busy)/sizeof(busy[0]);i++)if(has(s,busy[i]))return TY_HRI_BUSY;
 const char *ready[]={"我空了","我有空了","现在可以告诉我","说一下刚才的结果","告诉我刚才的结果","汇报结果","现在汇报","继续汇报","播报结果","把结果告诉我","把任务结果告诉我"};
 for(unsigned i=0;i<sizeof(ready)/sizeof(ready[0]);i++)if(has(s,ready[i]))return TY_HRI_AVAILABLE;
 /* 困了 must not silently authorize disclosure or replace an unspoken result. */
 if(pending&&(has(s,"我困了")||has(s,"我很困")||has(s,"我不困")))return TY_HRI_CLARIFY;
 if(has(s,"打扰到我")||has(s,"刚才打扰了"))return TY_HRI_INTRUSIVE;
 if(has(s,"刚才提醒有用")||has(s,"刚才提醒得很好"))return TY_HRI_HELPFUL;
 return TY_HRI_NONE;
}
