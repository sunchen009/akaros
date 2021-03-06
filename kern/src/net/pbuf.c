#include <ros/common.h>
#include <stdio.h>

#include <string.h>
#include <kmalloc.h>
#include <slab.h>
#include <assert.h>
#include <net/pbuf.h>
#include <sys/queue.h>
#include <net.h>
#include <debug.h>
#include <net/nic_common.h>


/* TODO: before running
 * 1. pbuf_free_auto currently decrefs the next one on the chain or queue
 * 2. copy_out currently copies out the chain following the next pointer
 *    could be dangerous if it runs on pbufs on a send/recv socket queue
 * 3. Tot_len could be useless at some point, especially if the max len is only two...
 * 4. pbuf_chain and pbuf_cat, pbuf_clen has no users yet
 */
#define MTU_PBUF_SIZE (sizeof(struct pbuf) + MAX_FRAME_SIZE + ETH_PAD_SIZE)

struct kmem_cache *pbuf_kcache;
struct kmem_cache *mtupbuf_kcache;


void pbuf_init(void){
	pbuf_kcache = kmem_cache_create("pbuf", sizeof(struct pbuf),
									__alignof__(struct pbuf), 0, 0, 0);
  mtupbuf_kcache = kmem_cache_create("mtupbuf_kcache", MTU_PBUF_SIZE, 
										__alignof__(struct pbuf), 0, 0, 0);
}

static void pbuf_free_auto(struct kref *kref){
    struct pbuf *p = container_of(kref, struct pbuf, bufref);
		if (!p) return;
		struct pbuf *q = STAILQ_NEXT(p, next);
		printd("deleting p %p of type %d\n", p, p->type);
    switch (p->type){
        case PBUF_REF:
            kmem_cache_free(pbuf_kcache, p);
            break;
        case PBUF_RAM:
            kfree(p);
            break;
				case PBUF_MTU:
						kmem_cache_free(mtupbuf_kcache,p);
						break;
        default:
            panic("Invalid pbuf type");
    }
		if (q != NULL) 
			pbuf_deref(q);
}

/**
 * Allocates a pbuf of the given type (possibly a chain for PBUF_POOL type).
 *
 * The actual memory allocated for the pbuf is determined by the
 * layer at which the pbuf is allocated and the requested size
 * (from the size parameter).
 *
 * @param layer flag to define header size
 * @param length size of the pbuf's payload
 * @param type this parameter decides how and where the pbuf
 * should be allocated as follows:
 *
 * - PBUF_RAM: buffer memory for pbuf is allocated as one large
 *             chunk. This includes protocol headers as well.
 * - PBUF_ROM: no buffer memory is allocated for the pbuf, even for
 *             protocol headers. Additional headers must be prepended
 *             by allocating another pbuf and chain in to the front of
 *             the ROM pbuf. It is assumed that the memory used is really
 *             similar to ROM in that it is immutable and will not be
 *             changed. Memory which is dynamic should generally not
 *             be attached to PBUF_ROM pbufs. Use PBUF_REF instead.
 * - PBUF_REF: no buffer memory is allocated for the pbuf, even for
 *             protocol headers. It is assumed that the pbuf is only
 *             being used in a single thread. If the pbuf gets queued,
 *             then pbuf_take should be called to copy the buffer.
 * - PBUF_MTU: specific to ROS, additional type that comes out of a 
 *             slab dedicated for the most common size (MTU sized) pbuf.
 *
 * @return the allocated pbuf. If multiple pbufs where allocated, this
 * is the first pbuf of a pbuf chain.
 */
struct pbuf *pbuf_alloc(pbuf_layer layer, uint16_t length, pbuf_type type)
{
  struct pbuf *p, *q, *r;
  uint16_t offset;
	uint16_t buf_size; 
  int rem_len; /* remaining length */

  /* determine header offset */
  offset = 0;
  switch (layer) {
  case PBUF_TRANSPORT:
    /* add room for transport (often TCP) layer header */
    offset += PBUF_TRANSPORT_HLEN;
    /* FALLTHROUGH */
  case PBUF_IP:
    /* add room for IP layer header */
    offset += PBUF_IP_HLEN;
    /* FALLTHROUGH */
  case PBUF_LINK:
    /* add room for link layer header */
    offset += PBUF_LINK_HLEN;
    break;
  case PBUF_RAW:
    break;
  default:
    warn("pbuf_alloc: bad pbuf layer", 0);
    return NULL;
  }
	
  switch (type) {
	case PBUF_MTU:
		/* special case PBUFs that are of a common size, notice the length has to be 0 in this case */
		assert(length==0); // TODO: reconsider this
    /* only allocate memory for the pbuf structure */
    p = (struct pbuf *)kmem_cache_alloc(mtupbuf_kcache, 0);
    if (p == NULL) {
      return NULL;
    }
    p->payload = (void *)((uint8_t *)p + sizeof(struct pbuf) + offset);
		STAILQ_NEXT(p, next) = NULL;
		p->type = type;
		p->alloc_len = MTU_PBUF_SIZE;
		p->len = p->tot_len = 0;
		break;

	case PBUF_RAM:
    /* If pbuf is to be allocated in RAM, allocate memory for it. */
    buf_size =  (sizeof(struct pbuf) + offset) + ROUNDUP(length, sizeof(void*));
    p = (struct pbuf*)kmalloc(buf_size, 0);

    if (p == NULL) {
      return NULL;
    }
    /* Set up internal structure of the pbuf. */
    p->payload = (void *)((uint8_t *)p + sizeof(struct pbuf) + offset);
    p->alloc_len = p->len = p->tot_len = length;
		STAILQ_NEXT(p, next) = NULL;
    p->type = type;
    break;
  case PBUF_REF:
    /* only allocate memory for the pbuf structure */
    p = (struct pbuf *)kmem_cache_alloc(pbuf_kcache, 0);
    if (p == NULL) {
      return NULL;
    }
    p->payload = NULL;
    p->alloc_len = p->len = p->tot_len = length;
		STAILQ_NEXT(p, next) = NULL;
    p->type = type;
    break;
	case PBUF_POOL:
		warn("POOL type not supported!");	
		return NULL;	
  default:
    warn("pbuf_alloc: wrong type", 0);
    return NULL;
  }
  kref_init(&p->bufref, pbuf_free_auto, 1); // TODO: pbuf_free
  /* set flags */
  p->flags = 0;
  return p;
}


/**
 * Shrink a pbuf chain to a desired length.
 *
 * @param p pbuf to shrink.
 * @param new_len desired new length of pbuf chain
 *
 * Depending on the desired length, the first few pbufs in a chain might
 * be skipped and left unchanged. The new last pbuf in the chain will be
 * resized, and any remaining pbufs will be freed.
 *
 * @note If the pbuf is ROM/REF, only the ->tot_len and ->len fields are adjusted.
 * @note May not be called on a packet queue.
 *
 * @note Despite its name, pbuf_realloc cannot grow the size of a pbuf (chain).
 */
void
pbuf_realloc(struct pbuf *p, uint16_t new_len)
{
  struct pbuf *q;
  uint16_t rem_len; /* remaining length */
  int32_t grow;

  /* desired length larger than current length? */
  if (new_len >= p->tot_len) {
    /* enlarging not yet supported */
    return;
  }

  /* the pbuf chain grows by (new_len - p->tot_len) bytes
   * (which may be negative in case of shrinking) */
  grow = new_len - p->tot_len;

  /* first, step over any pbufs that should remain in the chain */
  rem_len = new_len;
  q = p;
  /* should this pbuf be kept? */
  while (rem_len > q->len) {
    /* decrease remaining length by pbuf length */
    rem_len -= q->len;
    /* decrease total length indicator */
    LWIP_ASSERT("grow < max_uint16_t", grow < 0xffff);
    q->tot_len += (uint16_t)grow;
    /* proceed to next pbuf in chain */
		q = STAILQ_NEXT(q, next);
    LWIP_ASSERT("pbuf_realloc: q != NULL", q != NULL);
  }
  /* we have now reached the new last pbuf (in q) */
  /* rem_len == desired length for pbuf q */

  /* adjust length fields for new last pbuf */
  q->len = rem_len;
  q->tot_len = q->len;

  /* any remaining pbufs in chain? */
  if (STAILQ_NEXT(q, next) != NULL) {
    /* free remaining pbufs in chain */
		pbuf_free(STAILQ_NEXT(q, next));
  }
  /* q is last packet in chain */
	STAILQ_NEXT(q, next) = NULL;
}

void pbuf_ref(struct pbuf *p){
	kref_get(&p->bufref, 1);
}

/**
 * true if the pbuf is deallocated as a result of pbuf_deref
 * false means just a simple deref
 */
bool pbuf_deref(struct pbuf *p){
	return kref_put(&p->bufref);
}

void attach_pbuf(struct pbuf *p, struct pbuf_head *ph){
	spin_lock_irqsave(&ph->lock);
	ph->qlen++;
	pbuf_ref(p);
	STAILQ_INSERT_TAIL(&ph->pbuf_fifo, p, next);
	spin_unlock_irqsave(&ph->lock);
}

struct pbuf* detach_pbuf(struct pbuf_head *ph){
	struct pbuf* buf = NULL;
	if (ph->qlen == 0) return NULL;
	spin_lock_irqsave(&ph->lock);
	ph->qlen--;
	buf = STAILQ_FIRST(&ph->pbuf_fifo);
	STAILQ_REMOVE_HEAD(&ph->pbuf_fifo, next);
	spin_unlock_irqsave(&ph->lock);
	return buf;
}

/**
 * Copy (part of) the contents of a packet buffer
 * to an application supplied buffer.
 *
 * @param buf the pbuf from which to copy data
 * @param dataptr the application supplied buffer
 * @param len length of data to copy (dataptr must be big enough). No more 
 * than buf->tot_len will be copied, irrespective of len
 * @param offset offset into the packet buffer from where to begin copying len bytes
 * @return the number of bytes copied, or 0 on failure
 */
int pbuf_copy_out(struct pbuf *buf, void *dataptr, size_t len, uint16_t offset)
{
  struct pbuf *p;
  uint16_t left;
  uint16_t buf_copy_len;
  uint16_t copied_total = 0;
	
	if (dataptr == NULL || buf == NULL){
		warn("Copying a pbuf_copy to null pointer");
		return 0;
	}

  left = 0;
  for(p = buf; len != 0 && p != NULL; p = STAILQ_NEXT(p, next)) {
    if ((offset != 0) && (offset >= p->len)) {
      /* don't copy from this buffer -> on to the next */
      offset -= p->len;
    } else {
			/* offset is 0 now, start copying */
      buf_copy_len = p->len - offset;
      if (buf_copy_len > len)
          buf_copy_len = len;
      /* copy the necessary parts of the buffer */
      memcpy(&((char*)dataptr)[left], &((char*)p->payload)[offset], buf_copy_len);
      copied_total += buf_copy_len;
      left += buf_copy_len;
      len -= buf_copy_len;
      offset = 0;
    }
  }
  return copied_total;
}

/**
 * Chain two pbufs (or pbuf chains) together.
 * 
 * The caller MUST call pbuf_free(t) once it has stopped
 * using it. Use pbuf_cat() instead if you no longer use t.
 * 
 * @param h head pbuf (chain)
 * @param t tail pbuf (chain)
 * @note The pbufs MUST belong to the same packet.
 * @note MAY NOT be called on a packet queue.
 *
 * The ->tot_len fields of all pbufs of the head chain are adjusted.
 * The ->next field of the last pbuf of the head chain is adjusted.
 * The ->ref field of the first pbuf of the tail chain is adjusted.
 *
 */
void
pbuf_chain(struct pbuf *h, struct pbuf *t)
{
  pbuf_cat(h, t);
  /* t is now referenced by h */
  pbuf_ref(t);
}

void
pbuf_cat(struct pbuf *h, struct pbuf *t)
{
  struct pbuf *p;
  /* proceed to last pbuf of chain */
  for (p = h; STAILQ_NEXT(p, next) != NULL; p = STAILQ_NEXT(p, next)) {
    /* add total length of second chain to all totals of first chain */
    p->tot_len += t->tot_len;
  }
  /* add total length of second chain to last pbuf total of first chain */
  p->tot_len += t->tot_len;
  /* chain last pbuf of head (p) with first of tail (t) */
	STAILQ_NEXT(p,next) = t;
}

/**
 * Adjusts the payload pointer to hide or reveal headers in the payload.
 *
 * Adjusts the ->payload pointer so that space for a header
 * (dis)appears in the pbuf payload.
 *
 * The ->payload, ->tot_len and ->len fields are adjusted.
 *
 * @param p pbuf to change the header size.
 * @param header_size_increment Number of bytes to increment header size which
 * increases the size of the pbuf. New space is on the front.
 * (Using a negative value decreases the header size.)
 * If hdr_size_inc is 0, this function does nothing and returns succesful.
 *
 * PBUF_ROM and PBUF_REF type buffers cannot have their sizes increased, so
 * the call will fail. A check is made that the increase in header size does
 * not move the payload pointer in front of the start of the buffer.
 * @return non-zero on failure, zero on success.
 *
 */

/* TODO: when do we need to lock a pbuf?? */
int pbuf_header(struct pbuf *p, int delta){ // increase header size
	uint8_t type = p->type;
	void *payload = p->payload;
	if (p == NULL || delta == 0)
		return 0;
	// This assertion used to apply when len meant allocated space..
	// assert(-delta < p->len);

  /* pbuf types containing payloads? */
  if (type == PBUF_RAM || type == PBUF_POOL || type == PBUF_MTU) {
    /* set new payload pointer */
    p->payload = (uint8_t *)p->payload - delta;
    /* boundary check fails? */
    if ((uint8_t *)p->payload < (uint8_t *)p + sizeof(struct pbuf)) {
      /* restore old payload pointer */
      p->payload = payload;
			warn("boundary failed \n");
      /* bail out unsuccesfully */
      return 1;
    }
  /* pbuf types refering to external payloads? */
  } else if (type == PBUF_REF || type == PBUF_ROM) {
		/* header was embedded in the payload, we are extracting it */
    if ((delta < 0) && ((-delta) <= p->len)) {
      /* increase payload pointer */
      p->payload = (uint8_t *)p->payload - delta;
    } else {
      /* cannot expand payload to front (yet!)
       * bail out unsuccesfully */
      return 1;
    }
  } else {
    /* Unknown type */
    assert("bad pbuf type");
    return 1;
  }
  /* modify pbuf length fields */
  p->len += delta;
  p->tot_len += delta;

  return 0;
}

void print_pbuf(struct pbuf *p) {
	struct pbuf *next = p;
 	//basically while pbuf is not on the socket queue yet, we can't use STAILQ_NEXT 

	/*XXX: this is wrong.. */
	while (next != NULL) {
		printk("pbuf start \n");
		dumppacket(next->payload, next->len);
		printk("\n");
		next = STAILQ_NEXT(next, next);
	}
}


/**
 * Dereference a pbuf chain or queue and deallocate any no-longer-used
 * pbufs at the head of this chain or queue.
 *
 * Decrements the pbuf reference count. If it reaches zero, the pbuf is
 * deallocated.
 *
 * For a pbuf chain, this is repeated for each pbuf in the chain,
 * up to the first pbuf which has a non-zero reference count after
 * decrementing. So, when all reference counts are one, the whole
 * chain is free'd.
 *
 * @param p The pbuf (chain) to be dereferenced.
 *
 * @return the number of pbufs that were de-allocated
 * from the head of the chain.
 *
 *
 */
bool pbuf_free(struct pbuf *p) {
	return pbuf_deref(p);
}

/**
 * Count number of pbufs in a chain
 *
 * @param p first pbuf of chain
 * @return the number of pbufs in a chain
 */

uint8_t pbuf_clen(struct pbuf *p)
{
  uint8_t len;

  len = 0;
  while (p != NULL) {
    ++len;
    p = STAILQ_NEXT(p, next);
  }
  return len;
}


#if 0
#if LWIP_SUPPORT_CUSTOM_PBUF
/** Initialize a custom pbuf (already allocated).
 *
 * @param layer flag to define header size
 * @param length size of the pbuf's payload
 * @param type type of the pbuf (only used to treat the pbuf accordingly, as
 *        this function allocates no memory)
 * @param p pointer to the custom pbuf to initialize (already allocated)
 * @param payload_mem pointer to the buffer that is used for payload and headers,
 *        must be at least big enough to hold 'length' plus the header size,
 *        may be NULL if set later
 * @param payload_mem_len the size of the 'payload_mem' buffer, must be at least
 *        big enough to hold 'length' plus the header size
 */
struct pbuf*
pbuf_alloced_custom(pbuf_layer l, uint16_t length, pbuf_type type, struct pbuf_custom *p,
                    void *payload_mem, uint16_t payload_mem_len)
{
  uint16_t offset;
  LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_alloced_custom(length=%"U16_F")\n", length));

  /* determine header offset */
  offset = 0;
  switch (l) {
  case PBUF_TRANSPORT:
    /* add room for transport (often TCP) layer header */
    offset += PBUF_TRANSPORT_HLEN;
    /* FALLTHROUGH */
  case PBUF_IP:
    /* add room for IP layer header */
    offset += PBUF_IP_HLEN;
    /* FALLTHROUGH */
  case PBUF_LINK:
    /* add room for link layer header */
    offset += PBUF_LINK_HLEN;
    break;
  case PBUF_RAW:
    break;
  default:
    LWIP_ASSERT("pbuf_alloced_custom: bad pbuf layer", 0);
    return NULL;
  }

  if (LWIP_MEM_ALIGN_SIZE(offset) + length < payload_mem_len) {
    LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_LEVEL_WARNING, ("pbuf_alloced_custom(length=%"U16_F") buffer too short\n", length));
    return NULL;
  }

  p->pbuf.next = NULL;
  if (payload_mem != NULL) {
    p->pbuf.payload = LWIP_MEM_ALIGN((void *)((uint8_t *)payload_mem + offset));
  } else {
    p->pbuf.payload = NULL;
  }
  p->pbuf.flags = PBUF_FLAG_IS_CUSTOM;
  p->pbuf.len = p->pbuf.tot_len = length;
  p->pbuf.type = type;
  p->pbuf.ref = 1;
  return &p->pbuf;
}
#endif /* LWIP_SUPPORT_CUSTOM_PBUF */

/**
 * Shrink a pbuf chain to a desired length.
 *
 * @param p pbuf to shrink.
 * @param new_len desired new length of pbuf chain
 *
 * Depending on the desired length, the first few pbufs in a chain might
 * be skipped and left unchanged. The new last pbuf in the chain will be
 * resized, and any remaining pbufs will be freed.
 *
 * @note If the pbuf is ROM/REF, only the ->tot_len and ->len fields are adjusted.
 * @note May not be called on a packet queue.
 *
 * @note Despite its name, pbuf_realloc cannot grow the size of a pbuf (chain).
 */
void
pbuf_realloc(struct pbuf *p, uint16_t new_len)
{
  struct pbuf *q;
  uint16_t rem_len; /* remaining length */
  int32_t grow;

  LWIP_ASSERT("pbuf_realloc: p != NULL", p != NULL);
  LWIP_ASSERT("pbuf_realloc: sane p->type", p->type == PBUF_POOL ||
              p->type == PBUF_ROM ||
              p->type == PBUF_RAM ||
              p->type == PBUF_REF);

  /* desired length larger than current length? */
  if (new_len >= p->tot_len) {
    /* enlarging not yet supported */
    return;
  }

  /* the pbuf chain grows by (new_len - p->tot_len) bytes
   * (which may be negative in case of shrinking) */
  grow = new_len - p->tot_len;

  /* first, step over any pbufs that should remain in the chain */
  rem_len = new_len;
  q = p;
  /* should this pbuf be kept? */
  while (rem_len > q->len) {
    /* decrease remaining length by pbuf length */
    rem_len -= q->len;
    /* decrease total length indicator */
    LWIP_ASSERT("grow < max_uint16_t", grow < 0xffff);
    q->tot_len += (uint16_t)grow;
    /* proceed to next pbuf in chain */
    q = q->next;
    LWIP_ASSERT("pbuf_realloc: q != NULL", q != NULL);
  }
  /* we have now reached the new last pbuf (in q) */
  /* rem_len == desired length for pbuf q */

  /* shrink allocated memory for PBUF_RAM */
  /* (other types merely adjust their length fields */
  if ((q->type == PBUF_RAM) && (rem_len != q->len)) {
    /* reallocate and adjust the length of the pbuf that will be split */
    q = (struct pbuf *)mem_trim(q, (uint16_t)((uint8_t *)q->payload - (uint8_t *)q) + rem_len);
    LWIP_ASSERT("mem_trim returned q == NULL", q != NULL);
  }
  /* adjust length fields for new last pbuf */
  q->len = rem_len;
  q->tot_len = q->len;

  /* any remaining pbufs in chain? */
  if (q->next != NULL) {
    /* free remaining pbufs in chain */
    pbuf_free(q->next);
  }
  /* q is last packet in chain */
  q->next = NULL;

}



/**
 * Increment the reference count of the pbuf.
 *
 * @param p pbuf to increase reference counter of
 *
 */
void
pbuf_ref(struct pbuf *p)
{
  SYS_ARCH_DECL_PROTECT(old_level);
  /* pbuf given? */
  if (p != NULL) {
    SYS_ARCH_PROTECT(old_level);
    ++(p->ref);
    SYS_ARCH_UNPROTECT(old_level);
  }
}

/**
 * Concatenate two pbufs (each may be a pbuf chain) and take over
 * the caller's reference of the tail pbuf.
 * 
 * @note The caller MAY NOT reference the tail pbuf afterwards.
 * Use pbuf_chain() for that purpose.
 * 
 * @see pbuf_chain()
 */

void
pbuf_cat(struct pbuf *h, struct pbuf *t)
{
  struct pbuf *p;

  LWIP_ERROR("(h != NULL) && (t != NULL) (programmer violates API)",
             ((h != NULL) && (t != NULL)), return;);

  /* proceed to last pbuf of chain */
  for (p = h; p->next != NULL; p = p->next) {
    /* add total length of second chain to all totals of first chain */
    p->tot_len += t->tot_len;
  }
  /* { p is last pbuf of first h chain, p->next == NULL } */
  LWIP_ASSERT("p->tot_len == p->len (of last pbuf in chain)", p->tot_len == p->len);
  LWIP_ASSERT("p->next == NULL", p->next == NULL);
  /* add total length of second chain to last pbuf total of first chain */
  p->tot_len += t->tot_len;
  /* chain last pbuf of head (p) with first of tail (t) */
  p->next = t;
  /* p->next now references t, but the caller will drop its reference to t,
   * so netto there is no change to the reference count of t.
   */
}

/**
 * Chain two pbufs (or pbuf chains) together.
 * 
 * The caller MUST call pbuf_free(t) once it has stopped
 * using it. Use pbuf_cat() instead if you no longer use t.
 * 
 * @param h head pbuf (chain)
 * @param t tail pbuf (chain)
 * @note The pbufs MUST belong to the same packet.
 * @note MAY NOT be called on a packet queue.
 *
 * The ->tot_len fields of all pbufs of the head chain are adjusted.
 * The ->next field of the last pbuf of the head chain is adjusted.
 * The ->ref field of the first pbuf of the tail chain is adjusted.
 *
 */
void
pbuf_chain(struct pbuf *h, struct pbuf *t)
{
  pbuf_cat(h, t);
  /* t is now referenced by h */
  pbuf_ref(t);
  LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_chain: %p references %p\n", (void *)h, (void *)t));
}

/**
 * Dechains the first pbuf from its succeeding pbufs in the chain.
 *
 * Makes p->tot_len field equal to p->len.
 * @param p pbuf to dechain
 * @return remainder of the pbuf chain, or NULL if it was de-allocated.
 * @note May not be called on a packet queue.
 */
struct pbuf *
pbuf_dechain(struct pbuf *p)
{
  struct pbuf *q;
  uint8_t tail_gone = 1;
  /* tail */
  q = p->next;
  /* pbuf has successor in chain? */
  if (q != NULL) {
    /* assert tot_len invariant: (p->tot_len == p->len + (p->next? p->next->tot_len: 0) */
    LWIP_ASSERT("p->tot_len == p->len + q->tot_len", q->tot_len == p->tot_len - p->len);
    /* enforce invariant if assertion is disabled */
    q->tot_len = p->tot_len - p->len;
    /* decouple pbuf from remainder */
    p->next = NULL;
    /* total length of pbuf p is its own length only */
    p->tot_len = p->len;
    /* q is no longer referenced by p, free it */
    LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_dechain: unreferencing %p\n", (void *)q));
    tail_gone = pbuf_free(q);
    if (tail_gone > 0) {
      LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE,
                  ("pbuf_dechain: deallocated %p (as it is no longer referenced)\n", (void *)q));
    }
    /* return remaining tail or NULL if deallocated */
  }
  /* assert tot_len invariant: (p->tot_len == p->len + (p->next? p->next->tot_len: 0) */
  LWIP_ASSERT("p->tot_len == p->len", p->tot_len == p->len);
  return ((tail_gone > 0) ? NULL : q);
}

/**
 *
 * Create PBUF_RAM copies of pbufs.
 *
 * Used to queue packets on behalf of the lwIP stack, such as
 * ARP based queueing.
 *
 * @note You MUST explicitly use p = pbuf_take(p);
 *
 * @note Only one packet is copied, no packet queue!
 *
 * @param p_to pbuf destination of the copy
 * @param p_from pbuf source of the copy
 *
 * @return ERR_OK if pbuf was copied
 *         ERR_ARG if one of the pbufs is NULL or p_to is not big
 *                 enough to hold p_from
 */
err_t
pbuf_copy(struct pbuf *p_to, struct pbuf *p_from)
{
  uint16_t offset_to=0, offset_from=0, len;

  LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_copy(%p, %p)\n",
    (void*)p_to, (void*)p_from));

  /* is the target big enough to hold the source? */
  LWIP_ERROR("pbuf_copy: target not big enough to hold source", ((p_to != NULL) &&
             (p_from != NULL) && (p_to->tot_len >= p_from->tot_len)), return ERR_ARG;);

  /* iterate through pbuf chain */
  do
  {
    LWIP_ASSERT("p_to != NULL", p_to != NULL);
    /* copy one part of the original chain */
    if ((p_to->len - offset_to) >= (p_from->len - offset_from)) {
      /* complete current p_from fits into current p_to */
      len = p_from->len - offset_from;
    } else {
      /* current p_from does not fit into current p_to */
      len = p_to->len - offset_to;
    }
    MEMCPY((uint8_t*)p_to->payload + offset_to, (uint8_t*)p_from->payload + offset_from, len);
    offset_to += len;
    offset_from += len;
    LWIP_ASSERT("offset_to <= p_to->len", offset_to <= p_to->len);
    if (offset_to == p_to->len) {
      /* on to next p_to (if any) */
      offset_to = 0;
      p_to = p_to->next;
    }
    LWIP_ASSERT("offset_from <= p_from->len", offset_from <= p_from->len);
    if (offset_from >= p_from->len) {
      /* on to next p_from (if any) */
      offset_from = 0;
      p_from = p_from->next;
    }

    if((p_from != NULL) && (p_from->len == p_from->tot_len)) {
      /* don't copy more than one packet! */
      LWIP_ERROR("pbuf_copy() does not allow packet queues!\n",
                 (p_from->next == NULL), return ERR_VAL;);
    }
    if((p_to != NULL) && (p_to->len == p_to->tot_len)) {
      /* don't copy more than one packet! */
      LWIP_ERROR("pbuf_copy() does not allow packet queues!\n",
                  (p_to->next == NULL), return ERR_VAL;);
    }
  } while (p_from);
  LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_copy: end of chain reached.\n"));
  return ERR_OK;
}


/**
 * Copy application supplied data into a pbuf.
 * This function can only be used to copy the equivalent of buf->tot_len data.
 *
 * @param buf pbuf to fill with data
 * @param dataptr application supplied data buffer
 * @param len length of the application supplied data buffer
 *
 * @return ERR_OK if successful, ERR_MEM if the pbuf is not big enough
 */
err_t
pbuf_take(struct pbuf *buf, const void *dataptr, uint16_t len)
{
  struct pbuf *p;
  uint16_t buf_copy_len;
  uint16_t total_copy_len = len;
  uint16_t copied_total = 0;

  LWIP_ERROR("pbuf_take: invalid buf", (buf != NULL), return 0;);
  LWIP_ERROR("pbuf_take: invalid dataptr", (dataptr != NULL), return 0;);

  if ((buf == NULL) || (dataptr == NULL) || (buf->tot_len < len)) {
    return ERR_ARG;
  }

  /* Note some systems use byte copy if dataptr or one of the pbuf payload pointers are unaligned. */
  for(p = buf; total_copy_len != 0; p = p->next) {
    LWIP_ASSERT("pbuf_take: invalid pbuf", p != NULL);
    buf_copy_len = total_copy_len;
    if (buf_copy_len > p->len) {
      /* this pbuf cannot hold all remaining data */
      buf_copy_len = p->len;
    }
    /* copy the necessary parts of the buffer */
    MEMCPY(p->payload, &((char*)dataptr)[copied_total], buf_copy_len);
    total_copy_len -= buf_copy_len;
    copied_total += buf_copy_len;
  }
  LWIP_ASSERT("did not copy all data", total_copy_len == 0 && copied_total == len);
  return ERR_OK;
}

/**
 * Creates a single pbuf out of a queue of pbufs.
 *
 * @remark: Either the source pbuf 'p' is freed by this function or the original
 *          pbuf 'p' is returned, therefore the caller has to check the result!
 *
 * @param p the source pbuf
 * @param layer pbuf_layer of the new pbuf
 *
 * @return a new, single pbuf (p->next is NULL)
 *         or the old pbuf if allocation fails
 */
struct pbuf*
pbuf_coalesce(struct pbuf *p, pbuf_layer layer)
{
  struct pbuf *q;
  err_t err;
  if (p->next == NULL) {
    return p;
  }
  q = pbuf_alloc(layer, p->tot_len, PBUF_RAM);
  if (q == NULL) {
    /* @todo: what do we do now? */
    return p;
  }
  err = pbuf_copy(q, p);
  LWIP_ASSERT("pbuf_copy failed", err == ERR_OK);
  pbuf_free(p);
  return q;
}

#if LWIP_CHECKSUM_ON_COPY
/**
 * Copies data into a single pbuf (*not* into a pbuf queue!) and updates
 * the checksum while copying
 *
 * @param p the pbuf to copy data into
 * @param start_offset offset of p->payload where to copy the data to
 * @param dataptr data to copy into the pbuf
 * @param len length of data to copy into the pbuf
 * @param chksum pointer to the checksum which is updated
 * @return ERR_OK if successful, another error if the data does not fit
 *         within the (first) pbuf (no pbuf queues!)
 */
err_t
pbuf_fill_chksum(struct pbuf *p, uint16_t start_offset, const void *dataptr,
                 uint16_t len, uint16_t *chksum)
{
  u32_t acc;
  uint16_t copy_chksum;
  char *dst_ptr;
  LWIP_ASSERT("p != NULL", p != NULL);
  LWIP_ASSERT("dataptr != NULL", dataptr != NULL);
  LWIP_ASSERT("chksum != NULL", chksum != NULL);
  LWIP_ASSERT("len != 0", len != 0);

  if ((start_offset >= p->len) || (start_offset + len > p->len)) {
    return ERR_ARG;
  }

  dst_ptr = ((char*)p->payload) + start_offset;
  copy_chksum = LWIP_CHKSUM_COPY(dst_ptr, dataptr, len);
  if ((start_offset & 1) != 0) {
    copy_chksum = SWAP_BYTES_IN_WORD(copy_chksum);
  }
  acc = *chksum;
  acc += copy_chksum;
  *chksum = FOLD_U32T(acc);
  return ERR_OK;
}
#endif /* LWIP_CHECKSUM_ON_COPY */

 /** Get one byte from the specified position in a pbuf
 * WARNING: returns zero for offset >= p->tot_len
 *
 * @param p pbuf to parse
 * @param offset offset into p of the byte to return
 * @return byte at an offset into p OR ZERO IF 'offset' >= p->tot_len
 */
uint8_t
pbuf_get_at(struct pbuf* p, uint16_t offset)
{
  uint16_t copy_from = offset;
  struct pbuf* q = p;

  /* get the correct pbuf */
  while ((q != NULL) && (q->len <= copy_from)) {
    copy_from -= q->len;
    q = q->next;
  }
  /* return requested data if pbuf is OK */
  if ((q != NULL) && (q->len > copy_from)) {
    return ((uint8_t*)q->payload)[copy_from];
  }
  return 0;
}

/** Compare pbuf contents at specified offset with memory s2, both of length n
 *
 * @param p pbuf to compare
 * @param offset offset into p at wich to start comparing
 * @param s2 buffer to compare
 * @param n length of buffer to compare
 * @return zero if equal, nonzero otherwise
 *         (0xffff if p is too short, diffoffset+1 otherwise)
 */
uint16_t
pbuf_memcmp(struct pbuf* p, uint16_t offset, const void* s2, uint16_t n)
{
  uint16_t start = offset;
  struct pbuf* q = p;

  /* get the correct pbuf */
  while ((q != NULL) && (q->len <= start)) {
    start -= q->len;
    q = q->next;
  }
  /* return requested data if pbuf is OK */
  if ((q != NULL) && (q->len > start)) {
    uint16_t i;
    for(i = 0; i < n; i++) {
      uint8_t a = pbuf_get_at(q, start + i);
      uint8_t b = ((uint8_t*)s2)[i];
      if (a != b) {
        return i+1;
      }
    }
    return 0;
  }
  return 0xffff;
}

/** Find occurrence of mem (with length mem_len) in pbuf p, starting at offset
 * start_offset.
 *
 * @param p pbuf to search, maximum length is 0xFFFE since 0xFFFF is used as
 *        return value 'not found'
 * @param mem search for the contents of this buffer
 * @param mem_len length of 'mem'
 * @param start_offset offset into p at which to start searching
 * @return 0xFFFF if substr was not found in p or the index where it was found
 */
uint16_t
pbuf_memfind(struct pbuf* p, const void* mem, uint16_t mem_len, uint16_t start_offset)
{
  uint16_t i;
  uint16_t max = p->tot_len - mem_len;
  if (p->tot_len >= mem_len + start_offset) {
    for(i = start_offset; i <= max; ) {
      uint16_t plus = pbuf_memcmp(p, i, mem, mem_len);
      if (plus == 0) {
        return i;
      } else {
        i += plus;
      }
    }
  }
  return 0xFFFF;
}

/** Find occurrence of substr with length substr_len in pbuf p, start at offset
 * start_offset
 * WARNING: in contrast to strstr(), this one does not stop at the first \0 in
 * the pbuf/source string!
 *
 * @param p pbuf to search, maximum length is 0xFFFE since 0xFFFF is used as
 *        return value 'not found'
 * @param substr string to search for in p, maximum length is 0xFFFE
 * @return 0xFFFF if substr was not found in p or the index where it was found
 */
uint16_t
pbuf_strstr(struct pbuf* p, const char* substr)
{
  size_t substr_len;
  if ((substr == NULL) || (substr[0] == 0) || (p->tot_len == 0xFFFF)) {
    return 0xFFFF;
  }
  substr_len = strlen(substr);
  if (substr_len >= 0xFFFF) {
    return 0xFFFF;
  }
  return pbuf_memfind(p, substr, (uint16_t)substr_len, 0);
}

#endif /*EVERYTHING*/
