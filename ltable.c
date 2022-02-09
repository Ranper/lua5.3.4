/*
** $Id: ltable.c,v 2.118.1.4 2018/06/08 16:22:51 roberto Exp $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/

#define ltable_c
#define LUA_CORE

#include "lprefix.h"


/*
** Implementation of tables (aka arrays, objects, or hash tables).
** Tables keep its elements in two parts: an array part and a hash part.
** Non-negative integer keys are all candidates to be kept in the array
** part. The actual size of the array is the largest 'n' such that
** more than half the slots between 1 and n are in use.
** Hash uses a mix of chained scatter table with Brent's variation.
** A main invariant of these tables is that, if an element is not
** in its main position (i.e. the 'original' position that its hash gives
** to it), then the colliding element is in its own main position.
** Hence even when the load factor reaches 100%, performance remains good.
*/

#include <math.h>
#include <limits.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lvm.h"


/*
** Maximum size of array part (MAXASIZE) is 2^MAXABITS. MAXABITS is
** the largest integer such that MAXASIZE fits in an unsigned int.
*/
#define MAXABITS	cast_int(sizeof(int) * CHAR_BIT - 1)
#define MAXASIZE	(1u << MAXABITS)

/*
** Maximum size of hash part is 2^MAXHBITS. MAXHBITS is the largest
** integer such that 2^MAXHBITS fits in a signed int. (Note that the
** maximum number of elements in a table, 2^MAXABITS + 2^MAXHBITS, still
** fits comfortably in an unsigned int.)
*/
#define MAXHBITS	(MAXABITS - 1)


#define hashpow2(t,n)		(gnode(t, lmod((n), sizenode(t))))

#define hashstr(t,str)		hashpow2(t, (str)->hash)  // 找到 param2 在table t的哈希表中对应的位置的node
#define hashboolean(t,p)	hashpow2(t, p)
#define hashint(t,i)		hashpow2(t, i)


/*
** for some types, it is better to avoid modulus by power of 2, as
** they tend to have many 2 factors.
*/
#define hashmod(t,n)	(gnode(t, ((n) % ((sizenode(t)-1)|1))))


#define hashpointer(t,p)	hashmod(t, point2uint(p))


#define dummynode		(&dummynode_)

static const Node dummynode_ = {
  {NILCONSTANT},  /* value */
  {{NILCONSTANT, 0}}  /* key */
};


/*
** Hash for floating-point numbers.
** The main computation should be just
**     n = frexp(n, &i); return (n * INT_MAX) + i
** but there are some numerical subtleties.
** In a two-complement representation, INT_MAX does not has an exact
** representation as a float, but INT_MIN does; because the absolute
** value of 'frexp' is smaller than 1 (unless 'n' is inf/NaN), the
** absolute value of the product 'frexp * -INT_MIN' is smaller or equal
** to INT_MAX. Next, the use of 'unsigned int' avoids overflows when
** adding 'i'; the use of '~u' (instead of '-u') avoids problems with
** INT_MIN.
浮点数的散列。主计算应该是n=frexp（n，&i）；返回值（n*INT_MAX）+i，但在数值上有一些微妙之处。
在两个补码表示法中，INT_MAX没有精确的浮点表示法，但INT_MIN有；因为'frexp'的绝对值小于1（除非'n'是inf/NaN），
所以乘积'frexp*-INT_MIN'的绝对值小于或等于INT_MAX。接下来，使用'unsigned INT'可以避免在添加'i'时溢出；
使用“~u”（而不是“-u”）可以避免INT_MIN的问题。
*/
#if !defined(l_hashfloat)
static int l_hashfloat (lua_Number n) {
  int i;
  lua_Integer ni;
  n = l_mathop(frexp)(n, &i) * -cast_num(INT_MIN);
  if (!lua_numbertointeger(n, &ni)) {  /* is 'n' inf/-inf/NaN? */
    lua_assert(luai_numisnan(n) || l_mathop(fabs)(n) == cast_num(HUGE_VAL));
    return 0;
  }
  else {  /* normal case */
    unsigned int u = cast(unsigned int, i) + cast(unsigned int, ni);
    return cast_int(u <= cast(unsigned int, INT_MAX) ? u : ~u);
  }
}
#endif


/*
** returns the 'main' position of an element in a table (that is, the index
** of its hash value)
*/
// 在table 的哈希表中查找key的主位置 
// 返回值是一个在哈希表中的node 节点
static Node *mainposition (const Table *t, const TValue *key) {
  switch (ttype(key)) {
    case LUA_TNUMINT:
      return hashint(t, ivalue(key));  
    case LUA_TNUMFLT:
      return hashmod(t, l_hashfloat(fltvalue(key)));
    case LUA_TSHRSTR:
      return hashstr(t, tsvalue(key));
    case LUA_TLNGSTR:
      return hashpow2(t, luaS_hashlongstr(tsvalue(key)));
    case LUA_TBOOLEAN:
      return hashboolean(t, bvalue(key));
    case LUA_TLIGHTUSERDATA:
      return hashpointer(t, pvalue(key));
    case LUA_TLCF:
      return hashpointer(t, fvalue(key));
    default:
      lua_assert(!ttisdeadkey(key));
      return hashpointer(t, gcvalue(key));  // 如果都不是上述类型, 返回gc
  }
}


/*
适当的
** returns the index for 'key' if 'key' is an appropriate key to live in
** the array part of the table, 0 otherwise.
*/
static unsigned int arrayindex (const TValue *key) {
  if (ttisinteger(key)) {
    lua_Integer k = ivalue(key);  // 获取整数的值
    if (0 < k && (lua_Unsigned)k <= MAXASIZE)
      return cast(unsigned int, k);  /* 'key' is an appropriate array index */
  }
  return 0;  /* 'key' did not match some condition */
}


/*
traversals 遍历
** returns the index of a 'key' for table traversals. First goes all
** elements in the array part, then elements in the hash part. The
** beginning of a traversal is signaled by 0.
*/
// 现在数组部分遍历,再去hash表里遍历
static unsigned int findindex (lua_State *L, Table *t, StkId key) {
  unsigned int i;
  if (ttisnil(key)) return 0;  /* first iteration */ // 第一个迭代时,key为nil,返回0,遍历第一额元素
  i = arrayindex(key);  // key 必须是一个整数,然后将其转换为 unint ,并返回;否则,返回0
  if (i != 0 && i <= t->sizearray)  /* is 'key' inside array part? */   // key 不等于0 并且小于数组的大小,在数组部分,
    return i;  /* yes; that's the index */
  else {  // 否则就是在哈希表部分
    int nx;
    Node *n = mainposition(t, key);  // 获取key的主要位置. 如果主要位置上不是
    for (;;) {  /* check whether 'key' is somewhere in the chain */
      /* key may be dead already, but it is ok to use it in 'next' */
      if (luaV_rawequalobj(gkey(n), key) ||     // 当前节点的key跟要查询的key是不是一样, 变量活着的时候
            (ttisdeadkey(gkey(n)) && iscollectable(key) &&  // 变量死了的时候的判定方法
             deadvalue(gkey(n)) == gcvalue(key))) {     // 找到这个节点了.
        i = cast_int(n - gnode(t, 0));  /* key index in hash table */  // 获取要查询的key对应的节点在哈希表中的索引,通过位置偏移获取索引
        /* hash elements are numbered after array ones */ // 散列元素 在数组元素之后编号
        return (i + 1) + t->sizearray;  // 返回的索引是相对数组部分的索引. 从这里可以看出来,哈希表是紧跟在数组后面的.  
      }
      nx = gnext(n); // 如果主位置上不是要查询的key,根据next指针找下一个
      if (nx == 0)
        luaG_runerror(L, "invalid key to 'next'");  /* key not found */
      else n += nx;
    }
  }
}

// 传入上一个key,返回下一个键值对
// #TODO 既然i已经是找到的索引了,这里这里为什么要用循环呢? 因为这个函数是要找到给定key的下一个非空值,所以要遍历,直到找到一个非空的,
int luaH_next (lua_State *L, Table *t, StkId key) {
  unsigned int i = findindex(L, t, key);  /* find original element */     // 返回的索引是相对table  数组部分的索引, 如果key为空,就返回0,这里正好对应如果next的key为空,就返回第一个元素.因为0后面的元素就是1
  for (; i < t->sizearray; i++) {  /* try first array part */ // 先从数组里面找  这里直接返回了索引
    if (!ttisnil(&t->array[i])) {  /* a non-nil value? */  // 如果key不为空, 哈哈哈哈哈  pairs ipair 都是复用的next,next是调用了 luaH_next,luaH_next是根据value的值是不是空来判断这个键值对是否存在的.不是根据key,所以在table中,只要把value置为nil,就遍历不到了
      setivalue(key, i + 1);
      setobj2s(L, key+1, &t->array[i]);
      return 1;
    }
  }
  for (i -= t->sizearray; cast_int(i) < sizenode(t); i++) {  /* hash part */  // 再从哈希表里找
    if (!ttisnil(gval(gnode(t, i)))) {  /* a non-nil value? */
      setobj2s(L, key, gkey(gnode(t, i)));
      setobj2s(L, key+1, gval(gnode(t, i)));
      return 1;
    }
  }
  return 0;  /* no more elements */
}


/*
** {=============================================================
** Rehash
** ==============================================================
*/

/*
** Compute the optimal size for the array part of table 't'. 'nums' is a
** "count array" where 'nums[i]' is the number of integers in the table
** between 2^(i - 1) + 1 and 2^i. 'pna' enters with the total number of
** integer keys in the table and leaves with the number of keys that
** will go to the array part; return the optimal size.
*/
static unsigned int computesizes (unsigned int nums[], unsigned int *pna) {
  int i;
  unsigned int twotoi;  /* 2^i (candidate for optimal size) */
  unsigned int a = 0;  /* number of elements smaller than 2^i */
  unsigned int na = 0;  /* number of elements to go to array part */
  unsigned int optimal = 0;  /* optimal size for array part */
  /* loop while keys can fill more than half of total size */
  for (i = 0, twotoi = 1;
       twotoi > 0 && *pna > twotoi / 2;
       i++, twotoi *= 2) {
    if (nums[i] > 0) {
      a += nums[i];
      if (a > twotoi/2) {  /* more than half elements present? */
        optimal = twotoi;  /* optimal size (till now) */
        na = a;  /* all elements up to 'optimal' will go to array part */
      }
    }
  }
  lua_assert((optimal == 0 || optimal / 2 < na) && na <= optimal);
  *pna = na;
  return optimal;
}


static int countint (const TValue *key, unsigned int *nums) {
  unsigned int k = arrayindex(key);
  if (k != 0) {  /* is 'key' an appropriate array index? */
    nums[luaO_ceillog2(k)]++;  /* count as such */
    return 1;
  }
  else
    return 0;
}


/*
** Count keys in array part of table 't': Fill 'nums[i]' with
** number of keys that will go into corresponding slice and return
** total number of non-nil keys.
*/
static unsigned int numusearray (const Table *t, unsigned int *nums) {
  int lg;
  unsigned int ttlg;  /* 2^lg */
  unsigned int ause = 0;  /* summation of 'nums' */
  unsigned int i = 1;  /* count to traverse all array keys */
  /* traverse each slice */
  for (lg = 0, ttlg = 1; lg <= MAXABITS; lg++, ttlg *= 2) {
    unsigned int lc = 0;  /* counter */
    unsigned int lim = ttlg;
    if (lim > t->sizearray) {
      lim = t->sizearray;  /* adjust upper limit */
      if (i > lim)
        break;  /* no more elements to count */
    }
    /* count elements in range (2^(lg - 1), 2^lg] */
    for (; i <= lim; i++) {
      if (!ttisnil(&t->array[i-1]))
        lc++;
    }
    nums[lg] += lc;
    ause += lc;
  }
  return ause;
}


static int numusehash (const Table *t, unsigned int *nums, unsigned int *pna) {
  int totaluse = 0;  /* total number of elements */
  int ause = 0;  /* elements added to 'nums' (can go to array part) */
  int i = sizenode(t);
  while (i--) {
    Node *n = &t->node[i];
    if (!ttisnil(gval(n))) {
      ause += countint(gkey(n), nums);
      totaluse++;
    }
  }
  *pna += ause;
  return totaluse;
}


static void setarrayvector (lua_State *L, Table *t, unsigned int size) {
  unsigned int i;
  luaM_reallocvector(L, t->array, t->sizearray, size, TValue);
  for (i=t->sizearray; i<size; i++)  // 由于是要扩容,将扩出来的部分设置为nil(这里只处理了扩出来的部分)
     setnilvalue(&t->array[i]);
  t->sizearray = size;
}
/*
哈希表的最小尺寸为 2 的 0 次幂， 也就是 1 。 为了减少空表的维护 成本， lua 在这里做了一点优化。 
它定义了一个不可改写的空哈希表： dummynode 。 让空表被初始化时， Node 域指向这个 dummynode 节点。 
它虽然是一个全局变量，但因为对其访问是只读的，所以不会引起线程安全 问题。2
*/
// 初始化 table 的哈希表 部分.
static void setnodevector (lua_State *L, Table *t, unsigned int size) {
  if (size == 0) {  /* no elements to hash part? */
    t->node = cast(Node *, dummynode);  /* use common 'dummynode' */
    t->lsizenode = 0;
    t->lastfree = NULL;  /* signal that it is using dummy node */
  }
  else {
    int i;
    int lsize = luaO_ceillog2(size);
    if (lsize > MAXHBITS)
      luaG_runerror(L, "table overflow");
    size = twoto(lsize);
    t->node = luaM_newvector(L, size, Node);  // 申请大小为 size 的 Node 类型的数组
    for (i = 0; i < (int)size; i++) {
      Node *n = gnode(t, i);
      gnext(n) = 0;   // 将每个node 的 next 设置为0
      setnilvalue(wgkey(n));  // 将key 和value都设置nil
      setnilvalue(gval(n));
    }
    t->lsizenode = cast_byte(lsize);
    t->lastfree = gnode(t, size);  /* all positions are free */ // 最后一个可用的node, 在lastfree之后的node都是不可用的.
  }
}


typedef struct {
  Table *t;
  unsigned int nhsize;
} AuxsetnodeT;


static void auxsetnode (lua_State *L, void *ud) {
  AuxsetnodeT *asn = cast(AuxsetnodeT *, ud);
  setnodevector(L, asn->t, asn->nhsize);
}


void luaH_resize (lua_State *L, Table *t, unsigned int nasize,
                                          unsigned int nhsize) {
  unsigned int i;
  int j;
  AuxsetnodeT asn;
  unsigned int oldasize = t->sizearray;
  int oldhsize = allocsizenode(t);
  Node *nold = t->node;  /* save old hash ... */
  if (nasize > oldasize)  /* array part must grow? */
    setarrayvector(L, t, nasize);
  /* create new hash part with appropriate size */
  asn.t = t; asn.nhsize = nhsize;
  if (luaD_rawrunprotected(L, auxsetnode, &asn) != LUA_OK) {  /* mem. error? */
    setarrayvector(L, t, oldasize);  /* array back to its original size */
    luaD_throw(L, LUA_ERRMEM);  /* rethrow memory error */
  }

  // 将多余的部分 重新插入,由于key大于数组部分,所以会插入到哈希表部分
  if (nasize < oldasize) {  /* array part must shrink? */
    t->sizearray = nasize;    // 甚妙, 这个更改了 table的数组大小, 在luaH_setint 函数中重新插入的话,大于该值的key都会插入到哈希表部分,巧妙
    /* re-insert elements from vanishing slice */
    for (i=nasize; i<oldasize; i++) {
      if (!ttisnil(&t->array[i]))
        luaH_setint(L, t, i + 1, &t->array[i]);
    }
    /* shrink array */
    luaM_reallocvector(L, t->array, oldasize, nasize, TValue);  // 数组部分的缩减
  }


  /* re-insert elements from hash part */
  for (j = oldhsize - 1; j >= 0; j--) {
    Node *old = nold + j;
    if (!ttisnil(gval(old))) {
      /* doesn't need barrier/invalidate cache, as entry was
         already present in the table */
      setobjt2t(L, luaH_set(L, t, gkey(old)), gval(old));
    }
  }
  if (oldhsize > 0)  /* not the dummy node? */
    luaM_freearray(L, nold, cast(size_t, oldhsize)); /* free old hash */
}


void luaH_resizearray (lua_State *L, Table *t, unsigned int nasize) {
  int nsize = allocsizenode(t);
  luaH_resize(L, t, nasize, nsize);
}

/*
** nums[i] = number of keys 'k' where 2^(i - 1) < k <= 2^i
*/
static void rehash (lua_State *L, Table *t, const TValue *ek) {
  unsigned int asize;  /* optimal size for array part */
  unsigned int na;  /* number of keys in the array part */
  unsigned int nums[MAXABITS + 1];
  int i;
  int totaluse;
  for (i = 0; i <= MAXABITS; i++) nums[i] = 0;  /* reset counts */
  na = numusearray(t, nums);  /* count keys in array part */
  totaluse = na;  /* all those keys are integer keys */
  totaluse += numusehash(t, nums, &na);  /* count keys in hash part */
  /* count extra key */
  na += countint(ek, nums);
  totaluse++;
  /* compute new size for array part */
  asize = computesizes(nums, &na);  // computesizes函数计算出不低于50%利用率下，数组该维持多少空间。同时，还可以得到有多少有效键将被储存在哈希表里。
  /* resize the table to new computed sizes */
  luaH_resize(L, t, asize, totaluse - na);  // totaluse - na 表示在哈希表的部分
}



/*
** }=============================================================
*/
// 下面这两个函数调用的也都是 内存管理函数  realloc
// 申请table 空间,并初始化table
Table *luaH_new (lua_State *L) {
  GCObject *o = luaC_newobj(L, LUA_TTABLE, sizeof(Table));
  Table *t = gco2t(o);
  t->metatable = NULL;
  t->flags = cast_byte(~0);
  t->array = NULL;  // 数组部分为空
  t->sizearray = 0;  
  setnodevector(L, t, 0);  // 初始化哈希表部分
  return t;
}


void luaH_free (lua_State *L, Table *t) {
  if (!isdummy(t))  // 如果有必要释放 node节点
    luaM_freearray(L, t->node, cast(size_t, sizenode(t)));  // 释放node节点 
  luaM_freearray(L, t->array, t->sizearray);
  luaM_free(L, t);
}

// 从哈希表的后往前遍历找一个key为nil的node并返回
static Node *getfreepos (Table *t) {
  if (!isdummy(t)) {
    while (t->lastfree > t->node) {
      t->lastfree--;
      if (ttisnil(gkey(t->lastfree)))
        return t->lastfree;
    }
  }
  return NULL;  /* could not find a free place */
}


// colliding means 碰撞
// 只负责在哈希 表中创建出一个不存在的键，而不关数组部分的工作。
/*
** inserts a new key into a hash table; first, check whether key's main
** position is free. If not, check whether colliding node is in its main
** position or not: if it is not, move colliding node to an empty place and
** put new key in its main position; otherwise (colliding node is in its main
** position), new key goes to an empty position.
*/
// 当有冲突时, 使用闭散列的方式     字符串使用的开放散裂,hash冲突了,直接使用列表
/*
lua 的哈希表以闭散列方式实现。 每个可能的键值，在哈希表中都有一个主要位置，称作 main position。 比如a元素的hash值是2,放到2的位置,2就是a的主位置.b的hash值也是2,但是2已经有a了,并且还是主位置,那b只能找下一个空闲的位置,比如3,c的位置是3,这时候3的位置已经有b了,但并不是b的主位置,所以b再找一个空闲的位置,c放到3的位置上.
 创建一个新键时，检查 main position ，若无人使用，则可以直接设置为这个新键。 若之前有其它键占据了
这个位置，则检查占据此位置的键的主位置是不是这里。 若两者位置冲突，则利用 node 结构中的 next 域， 
以一个单向链表的形式把它们链起来；否则，新键占据这个位置，而老键更换到新位置并根据它的主键找到 
属于它的链的那条单向链表中上一个结点，重新链入。

*/
TValue *luaH_newkey (lua_State *L, Table *t, const TValue *key) {
  Node *mp;
  TValue aux;
  if (ttisnil(key)) luaG_runerror(L, "table index is nil");   // key不可以为nil
  else if (ttisfloat(key)) {
    lua_Integer k;
    if (luaV_tointeger(key, &k, 0)) {  /* does index fit in an integer? */  // 如果是key是float, 将其转换为整数 ? 不对吧,这里的mode=0
      setivalue(&aux, k);
      key = &aux;  /* insert it as an integer */
    }
    else if (luai_numisnan(fltvalue(key)))
      luaG_runerror(L, "table index is NaN");
  }


  mp = mainposition(t, key);  // 找到key对应的主位置
  if (!ttisnil(gval(mp)) || isdummy(t)) {  /* main position is taken? */
    Node *othern;
    Node *f = getfreepos(t);  /* get a free place */
    if (f == NULL) {  /* cannot find a free place? */
      rehash(L, t, key);  /* grow table */
      /* whatever called 'newkey' takes care of TM cache */
      return luaH_set(L, t, key);  /* insert key into grown table */
    }
    lua_assert(!isdummy(t));
    othern = mainposition(t, gkey(mp));
    if (othern != mp) {  /* is colliding node out of its main position? */
      /* yes; move colliding node into free position */
      while (othern + gnext(othern) != mp)  /* find previous */
        othern += gnext(othern);
      gnext(othern) = cast_int(f - othern);  /* rechain to point to 'f' */
      *f = *mp;  /* copy colliding node into free pos. (mp->next also goes) */
      if (gnext(mp) != 0) {
        gnext(f) += cast_int(mp - f);  /* correct 'next' */
        gnext(mp) = 0;  /* now 'mp' is free */
      }
      setnilvalue(gval(mp));
    }
    else {  /* colliding node is in its own main position */
      /* new node will go into free position */
      if (gnext(mp) != 0)
        gnext(f) = cast_int((mp + gnext(mp)) - f);  /* chain new position */
      else lua_assert(gnext(f) == 0);
      gnext(mp) = cast_int(f - mp);
      mp = f;
    }
  }
  setnodekey(L, &mp->i_key, key);
  luaC_barrierback(L, t, key);
  lua_assert(ttisnil(gval(mp)));
  return gval(mp);
}


/*
** search function for integers
*/
const TValue *luaH_getint (Table *t, lua_Integer key) {
  /* (1 <= key && key <= t->sizearray) */
  if (l_castS2U(key) - 1 < t->sizearray)
    return &t->array[key - 1];
  else {
    Node *n = hashint(t, key);
    for (;;) {  /* check whether 'key' is somewhere in the chain */
      if (ttisinteger(gkey(n)) && ivalue(gkey(n)) == key)
        return gval(n);  /* that's it */
      else {
        int nx = gnext(n);
        if (nx == 0) break;
        n += nx;
      }
    }
    return luaO_nilobject;
  }
}


/*
** search function for short strings
*/
const TValue *luaH_getshortstr (Table *t, TString *key) {
  Node *n = hashstr(t, key);  // 短字符串的哈希值在table 哈希表部分对应的值
  lua_assert(key->tt == LUA_TSHRSTR);
  for (;;) {  /* check whether 'key' is somewhere in the chain */
    const TValue *k = gkey(n);
    if (ttisshrstring(k) && eqshrstr(tsvalue(k), key))
      return gval(n);  /* that's it */
    else {
      int nx = gnext(n);  // 如果当前位置不是该短字符串,去链表里面寻找
      if (nx == 0)
        return luaO_nilobject;  /* not found */
      n += nx;
    }
  }
}


/*
** "Generic" get version. (Not that generic: not valid for integers,
** which may be in array part, nor for floats with integral values.)
*/
static const TValue *getgeneric (Table *t, const TValue *key) {
  Node *n = mainposition(t, key);
  for (;;) {  /* check whether 'key' is somewhere in the chain */
    if (luaV_rawequalobj(gkey(n), key))
      return gval(n);  /* that's it */
    else {
      int nx = gnext(n);
      if (nx == 0)
        return luaO_nilobject;  /* not found */
      n += nx;
    }
  }
}


const TValue *luaH_getstr (Table *t, TString *key) {
  if (key->tt == LUA_TSHRSTR)
    return luaH_getshortstr(t, key);
  else {  /* for long strings, use generic case */
    TValue ko;
    setsvalue(cast(lua_State *, NULL), &ko, key);
    return getgeneric(t, &ko);
  }
}


/*
** main search function
*/
// 查询操作 luaH_get 的实现要简单的多。当查询键为整数键目在数组范围内时，在数组部分查询:否则根据键的哈希值去哈希表中查询。拥有相同哈希值的冲突键值对，在哈希表中由 Node 的 next 域单向链起来，所以遍历这个链表就可以了。
const TValue *luaH_get (Table *t, const TValue *key) {
  switch (ttype(key)) {
    case LUA_TSHRSTR: return luaH_getshortstr(t, tsvalue(key));
    case LUA_TNUMINT: return luaH_getint(t, ivalue(key));
    case LUA_TNIL: return luaO_nilobject;
    case LUA_TNUMFLT: {
      lua_Integer k;
      if (luaV_tointeger(key, &k, 0)) /* index is int? */
        return luaH_getint(t, k);  /* use specialized version */
      /* else... */
    }  /* FALLTHROUGH */
    default:
      return getgeneric(t, key);
  }
}


/*
** beware: when using this function you probably need to check a GC
** barrier and invalidate the TM cache.
*/
TValue *luaH_set (lua_State *L, Table *t, const TValue *key) {
  const TValue *p = luaH_get(t, key);
  if (p != luaO_nilobject)  // 不为nil
    return cast(TValue *, p);
  else return luaH_newkey(L, t, key);
}

// 将t 的 key 对应的value 设置为 value
void luaH_setint (lua_State *L, Table *t, lua_Integer key, TValue *value) {
  const TValue *p = luaH_getint(t, key);  // 获取key 对应的value:如果小于数组的长度,返回key-1,否则去哈希表里遍历查找
  TValue *cell;  
  if (p != luaO_nilobject)  // value 不为空,说明key上有值,不需要新建,只要覆盖就行
    cell = cast(TValue *, p);
  else {  // value为空,说明 key不存在,需要新建key 并插入
    TValue k;
    setivalue(&k, key);  // 新建一个整数型变量
    cell = luaH_newkey(L, t, &k);   // 这时候时不考虑Tvalue是不是nil,所以哪怕新插入的键值对的值为nil,也可能进行rehash
  }
  setobj2t(L, cell, value);
}


static lua_Unsigned unbound_search (Table *t, lua_Unsigned j) {
  lua_Unsigned i = j;  /* i is zero or a present index */
  j++;
  /* find 'i' and 'j' such that i is present and j is not */
  while (!ttisnil(luaH_getint(t, j))) {
    i = j;
    if (j > l_castS2U(LUA_MAXINTEGER) / 2) {  /* overflow? */
      /* table was built with bad purposes: resort to linear search */
      i = 1;
      while (!ttisnil(luaH_getint(t, i))) i++;
      return i - 1;
    }
    j *= 2;
  }
  /* now do a binary search between them */
  while (j - i > 1) {
    lua_Unsigned m = (i+j)/2;
    if (ttisnil(luaH_getint(t, m))) j = m;
    else i = m;
  }
  return i;
}


/*
** Try to find a boundary in table 't'. A 'boundary' is an integer index
** such that t[i] is non-nil and t[i+1] is nil (and 0 if t[1] is nil).
*/
lua_Unsigned luaH_getn (Table *t) {
  unsigned int j = t->sizearray;
  if (j > 0 && ttisnil(&t->array[j - 1])) {
    /* there is a boundary in the array part: (binary) search for it */
    unsigned int i = 0;
    while (j - i > 1) {
      unsigned int m = (i+j)/2;
      if (ttisnil(&t->array[m - 1])) j = m;
      else i = m;
    }
    return i;
  }
  /* else must find a boundary in hash part */
  else if (isdummy(t))  /* hash part is empty? */
    return j;  /* that is easy... */
  else return unbound_search(t, j);
}



#if defined(LUA_DEBUG)

Node *luaH_mainposition (const Table *t, const TValue *key) {
  return mainposition(t, key);
}

int luaH_isdummy (const Table *t) { return isdummy(t); }

#endif
