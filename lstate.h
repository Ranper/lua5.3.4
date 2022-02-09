/*
** $Id: lstate.h,v 2.133.1.1 2017/04/19 17:39:34 roberto Exp $
** Global State
** See Copyright Notice in lua.h
*/

#ifndef lstate_h
#define lstate_h

#include "lua.h"

#include "lobject.h"
#include "ltm.h"
#include "lzio.h"


/*

** Some notes about garbage-collected objects: All objects in Lua must
** be kept somehow accessible until being freed, so all objects always
** belong to one (and only one) of these lists, using field 'next' of
** the 'CommonHeader' for the link:
**
** 'allgc': all objects not marked for finalization;
** 'finobj': all objects marked for finalization;
** 'tobefnz': all objects ready to be finalized;
** 'fixedgc': all objects that are not to be collected (currently
** only small strings, such as reserved words).
**
** Moreover, there is another set of lists that control gray objects.
** These lists are linked by fields 'gclist'. (All objects that
** can become gray have such a field. The field is not the same
** in all objects, but it always has this name.)  Any gray object
** must belong to one of these lists, and all objects in these lists
** must be gray:
**
** 'gray': regular gray objects, still waiting to be visited.
** 'grayagain': objects that must be revisited at the atomic phase.
**   That includes
**   - black objects got in a write barrier;
**   - all kinds of weak tables during propagation phase;
**   - all threads.
** 'weak': tables with weak values to be cleared;
** 'ephemeron': ephemeron tables with white->white entries;
** 'allweak': tables with weak keys and/or weak values to be cleared.
** The last three lists are used only during the atomic phase.

*/


struct lua_longjmp;  /* defined in ldo.c */


/*
** Atomic type (relative to signals) to better ensure that 'lua_sethook'
** is thread safe
*/
#if !defined(l_signalT)
#include <signal.h>
#define l_signalT	sig_atomic_t
#endif


/* extra stack space to handle TM calls and some other extras */
#define EXTRA_STACK   5


#define BASIC_STACK_SIZE        (2*LUA_MINSTACK)


/* kinds of Garbage Collection */
#define KGC_NORMAL	0
#define KGC_EMERGENCY	1	/* gc was forced by an allocation failure */

// 存放字符串的hash表.
typedef struct stringtable {
  TString **hash;  // 二级指针  思考一下这里为什么是二级指针
  int nuse;  /* number of elements */  // 用了多少个坑
  int size;  // 共有多少个坑
} stringtable;


/*
** Information about a call.
** When a thread yields, 'func' is adjusted to pretend that the
** top function has only the yielded values in its stack; in that
** case, the actual 'func' value is saved in field 'extra'.
** When a function calls another with a continuation, 'extra' keeps
** the function index so that, in case of errors, the continuation
** function can be called with the correct top.
*/
typedef struct CallInfo {
  StkId func;  /* function index in the stack */
  StkId	top;  /* top for this function */
  struct CallInfo *previous, *next;  /* dynamic call link */
  union {
    struct {  /* only for Lua functions */
      StkId base;  /* base for this function */
      const Instruction *savedpc;
    } l;
    struct {  /* only for C functions */
      lua_KFunction k;  /* continuation in case of yields */
      ptrdiff_t old_errfunc;
      lua_KContext ctx;  /* context info. in case of yields */
    } c;
  } u;
  ptrdiff_t extra;
  short nresults;  /* expected number of results from this function */
  unsigned short callstatus;
} CallInfo;


/*
** Bits in CallInfo status
*/
#define CIST_OAH	(1<<0)	/* original value of 'allowhook' */
#define CIST_LUA	(1<<1)	/* call is running a Lua function */
#define CIST_HOOKED	(1<<2)	/* call is running a debug hook */
#define CIST_FRESH	(1<<3)	/* call is running on a fresh invocation
                                   of luaV_execute */
#define CIST_YPCALL	(1<<4)	/* call is a yieldable protected call */
#define CIST_TAIL	(1<<5)	/* call was tail called */
#define CIST_HOOKYIELD	(1<<6)	/* last hook called yielded */
#define CIST_LEQ	(1<<7)  /* using __lt for __le */
#define CIST_FIN	(1<<8)  /* call is running a finalizer */

#define isLua(ci)	((ci)->callstatus & CIST_LUA)

/* assume that CIST_OAH has offset 0 and that 'v' is strictly 0/1 */
#define setoah(st,v)	((st) = ((st) & ~CIST_OAH) | (v))
#define getoah(st)	((st) & CIST_OAH)


/*
** 'global state', shared by all threads of this state
*/
// 全局状态机 所有线程共享
typedef struct global_State {
  lua_Alloc frealloc;  /* function to reallocate memory */    // 内存管理函数, 不仅仅是分配新的内存，释放不用的内存，扩展不够用 的内存。Lua 也会通过 realloc 试图释放掉预申请过大的内存的后半部分. 这就是为什么使用realloc,不使用alloc
  void *ud;         /* auxiliary data to 'frealloc' */  // 额外指针ud,可以让内存管理模块工作在不同的堆上
  l_mem totalbytes;  /* number of bytes currently allocated - GCdebt */  // 当前分配的字节数-GC 借款
  l_mem GCdebt;  /* bytes allocated not yet compensated by the collector */  // 收集器尚未补偿分配的字节 ??
  lu_mem GCmemtrav;  /* memory traversed by the GC */            // GC遍历的内存
  lu_mem GCestimate;  /* an estimate of the non-garbage memory in use */  // 对正在使用的非垃圾内存的估计
  stringtable strt;  /* hash table for strings */    // 字符串哈希表 string table 短字符串都存放在这个hash表中
  TValue l_registry;
  unsigned int seed;  /* randomized seed for hashes */  // 散列随机种子
  lu_byte currentwhite;
  lu_byte gcstate;  /* state of garbage collector */  // 垃圾收集器的状态
  lu_byte gckind;  /* kind of GC running */         // gc 运行的种类
  lu_byte gcrunning;  /* true if GC is running */  // 标志gc是否在运行
  GCObject *allgc;  /* list of all collectable objects */ // 可回收对象的列表
  GCObject **sweepgc;  /* current position of sweep in list */  // 扫描列表的当前位置
  GCObject *finobj;  /* list of collectable objects with finalizers */  // 带有终结器的可收集对象列表  finalizers应该类似于析构函数
  GCObject *gray;  /* list of gray objects */                            // 灰色对象列表
  GCObject *grayagain;  /* list of objects to be traversed atomically */  // 要以原子方式遍历的对象列表
  GCObject *weak;  /* list of tables with weak values */  // 弱引用的table 列表
  GCObject *ephemeron;  /* list of ephemeron tables (weak keys) */  // 弱键
  GCObject *allweak;  /* list of all-weak tables */   // 所有的弱键列表
  GCObject *tobefnz;  /* list of userdata to be GC */   // 需要gc的用户数据的列表
  GCObject *fixedgc;  /* list of objects not to be collected */   // 不收集的对象列表
  struct lua_State *twups;  /* list of threads with open upvalues */   //具有开放上值的线程列表
  unsigned int gcfinnum;  /* number of finalizers to call in each GC step */   // 每个GC步骤中要调用的终结器数
  int gcpause;  /* size of pause between successive GCs */  // 连续GCs之间的暂停大小
  int gcstepmul;  /* GC 'granularity' */   // gc粒度
  lua_CFunction panic;  /* to be called in unprotected errors */   // 惊恐  在不受保护的错误中调用
  struct lua_State *mainthread;   // 主线程的引用
  const lua_Number *version;  /* pointer to version number */  // 指向版本号的指针
  TString *memerrmsg;  /* memory-error message */   // 内存错误消息 
  TString *tmname[TM_N];  /* array with tag-method names */  // 带有标记方法名称的数组
  struct Table *mt[LUA_NUMTAGS];  /* metatables for basic types */   // 基础类型的元表
  TString *strcache[STRCACHE_N][STRCACHE_M];  /* cache for strings in API */  // API中字符串的缓存
} global_State;


/*
** 'per thread' state
*/
// 每个线程的状态  一个栈就是一个线程
struct lua_State {
  CommonHeader;
  unsigned short nci;  /* number of items in 'ci' list */  // “ci”列表中的项目数  ci for call info 
  lu_byte status;
  StkId top;  /* first free slot in the stack */    //栈上第一个空闲位的索引
  global_State *l_G;   // 全局状态引用
  CallInfo *ci;  /* call info for current function */  //当前函数的调用信息
  const Instruction *oldpc;  /* last pc traced */  // 上一个程序计数器?
  StkId stack_last;  /* last free slot in the stack */
  StkId stack;  /* stack base */  
  UpVal *openupval;  /* list of open upvalues in this stack */  // 这个栈中开放的上值,以单项链表的形式串接起来
  GCObject *gclist;
  struct lua_State *twups;  /* list of threads with open upvalues */  // 拥有open value的线程列表 
  struct lua_longjmp *errorJmp;  /* current error recover point */  // 
  CallInfo base_ci;  /* CallInfo for first level (C calling Lua) */  // c调用lua的调用信息
  volatile lua_Hook hook;   // hook???
  ptrdiff_t errfunc;  /* current error handling function (stack index) */   // 当前的错误处理函数, 栈索引?
  int stacksize;  
  int basehookcount;
  int hookcount;
  unsigned short nny;  /* number of non-yieldable calls in stack */   // 堆栈中不可接受的调用数
  unsigned short nCcalls;  /* number of nested C calls */
  l_signalT hookmask;
  lu_byte allowhook;
};


#define G(L)	(L->l_G)


/*
** Union of all collectable objects (only for conversions)
*/
union GCUnion {
  GCObject gc;  /* common header */
  struct TString ts;
  struct Udata u;
  union Closure cl;
  struct Table h;
  struct Proto p;
  struct lua_State th;  /* thread */
};


#define cast_u(o)	cast(union GCUnion *, (o))

/* macros to convert a GCObject into a specific value */
#define gco2ts(o)  \
	check_exp(novariant((o)->tt) == LUA_TSTRING, &((cast_u(o))->ts))
#define gco2u(o)  check_exp((o)->tt == LUA_TUSERDATA, &((cast_u(o))->u))
#define gco2lcl(o)  check_exp((o)->tt == LUA_TLCL, &((cast_u(o))->cl.l))
#define gco2ccl(o)  check_exp((o)->tt == LUA_TCCL, &((cast_u(o))->cl.c))
#define gco2cl(o)  \
	check_exp(novariant((o)->tt) == LUA_TFUNCTION, &((cast_u(o))->cl))
#define gco2t(o)  check_exp((o)->tt == LUA_TTABLE, &((cast_u(o))->h))
#define gco2p(o)  check_exp((o)->tt == LUA_TPROTO, &((cast_u(o))->p))
#define gco2th(o)  check_exp((o)->tt == LUA_TTHREAD, &((cast_u(o))->th))


/* macro to convert a Lua object into a GCObject */
#define obj2gco(v) \
	check_exp(novariant((v)->tt) < LUA_TDEADKEY, (&(cast_u(v)->gc)))


/* actual number of total bytes allocated */
#define gettotalbytes(g)	cast(lu_mem, (g)->totalbytes + (g)->GCdebt)

LUAI_FUNC void luaE_setdebt (global_State *g, l_mem debt);
LUAI_FUNC void luaE_freethread (lua_State *L, lua_State *L1);
LUAI_FUNC CallInfo *luaE_extendCI (lua_State *L);
LUAI_FUNC void luaE_freeCI (lua_State *L);
LUAI_FUNC void luaE_shrinkCI (lua_State *L);


#endif

