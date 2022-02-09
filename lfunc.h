/*
** $Id: lfunc.h,v 2.15.1.1 2017/04/19 17:39:34 roberto Exp $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/

#ifndef lfunc_h
#define lfunc_h


#include "lobject.h"


#define sizeCclosure(n)	(cast(int, sizeof(CClosure)) + \
                         cast(int, sizeof(TValue)*((n)-1)))

#define sizeLclosure(n)	(cast(int, sizeof(LClosure)) + \
                         cast(int, sizeof(TValue *)*((n)-1)))


/* test whether thread is in 'twups' list */
#define isintwups(L)	(L->twups != L)


/*
** maximum number of upvalues in a closure (both C and Lua). (Value
** must fit in a VM register.)
*/
#define MAXUPVAL	255


/*
** Upvalues for Lua closures
*/
/*
它直接用一个TValue 指针引用一个 Lua 值变量。当被引用的变量还在数据栈上时，这个指针直接指向栈上的地址。
这个 upvalue 被称为开放的。由于 Lua的数据栈的大小可扩展，当数据栈内存延展时，其内存地址会发生变化。
这个时候需要主动修正UpVal结构中的指针。这个过程我们在6.1.1节提及，它由ldo.c中的correctstack 函数来实现5。
遍历当前所有开放的upvalue利用的是当前线程中记录的链表 openupvale 。这是一个双向链表，所以在 UpVal 结构中有两个指针指向前后节点。5.3里面为单向链表
链表指针保存在一个联合中。当upvalue 被关闭后，就不再需要这两个指针了。所谓关闭upvalue，就是当upvalue引用的数据栈上的数据不再存在于栈上时(通常是由申请局部变量的函数返回引起的)，需要把 upvalue 从开放链表中拿掉，并把其引用的数据栈上的变量值换一个安全的地方存放。这个安全所在就是 UpVal 结构体内。
无须用特别的标记位区分一个UpVal在开放还是关闭状态。当upvalue 关闭时，UpVal 中的指针v一定指向结构内部的value。
*/
//开放的upvalue 指的是 该upvalue引用的变量还在数据栈上,外层函数还没有退出.
struct UpVal {
  TValue *v;  /* points to stack or to its own value */
  lu_mem refcount;  /* reference counter */ // 引用计数,当没有人引用的话,就释放掉
  union {
    struct {  /* (when open) */  // 当upvalue打开的时候, TValue指针 指向open结构体
      UpVal *next;  /* linked list */  
      int touched;  /* mark to avoid cycles with dead threads */  // 标记以避免出现死线循环
    } open;
    TValue value;  /* the value (when closed) */ // 当upvalue关闭的时候, TValue 指针指向这个结构体
  }u ;
};

#define upisopen(up)	((up)->v != &(up)->u.value)


LUAI_FUNC Proto *luaF_newproto (lua_State *L);
LUAI_FUNC CClosure *luaF_newCclosure (lua_State *L, int nelems);
LUAI_FUNC LClosure *luaF_newLclosure (lua_State *L, int nelems);
LUAI_FUNC void luaF_initupvals (lua_State *L, LClosure *cl);
LUAI_FUNC UpVal *luaF_findupval (lua_State *L, StkId level);
LUAI_FUNC void luaF_close (lua_State *L, StkId level);
LUAI_FUNC void luaF_freeproto (lua_State *L, Proto *f);
LUAI_FUNC const char *luaF_getlocalname (const Proto *func, int local_number,
                                         int pc);


#endif
