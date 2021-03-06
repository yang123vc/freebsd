/*
 * Copyright © 2010 Daniel Vetter
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <dev/drm2/drmP.h>
#include <dev/drm2/drm.h>
#include <dev/drm2/i915/i915_drm.h>
#include <dev/drm2/i915/i915_drv.h>
#include <dev/drm2/i915/intel_drv.h>
#include <sys/sched.h>
#include <sys/sf_buf.h>

/* PPGTT support for Sandybdrige/Gen6 and later */
static void i915_ppgtt_clear_range(struct i915_hw_ppgtt *ppgtt,
				   unsigned first_entry,
				   unsigned num_entries)
{
	uint32_t *pt_vaddr;
	uint32_t scratch_pte;
	struct sf_buf *sf;
	unsigned act_pd, first_pte, last_pte, i;

	act_pd = first_entry / I915_PPGTT_PT_ENTRIES;
	first_pte = first_entry % I915_PPGTT_PT_ENTRIES;

	scratch_pte = GEN6_PTE_ADDR_ENCODE(ppgtt->scratch_page_dma_addr);
	scratch_pte |= GEN6_PTE_VALID | GEN6_PTE_CACHE_LLC;

	while (num_entries) {
		last_pte = first_pte + num_entries;
		if (last_pte > I915_PPGTT_PT_ENTRIES)
			last_pte = I915_PPGTT_PT_ENTRIES;

		sched_pin();
		sf = sf_buf_alloc(ppgtt->pt_pages[act_pd], SFB_CPUPRIVATE);
		pt_vaddr = (uint32_t *)(uintptr_t)sf_buf_kva(sf);

		for (i = first_pte; i < last_pte; i++)
			pt_vaddr[i] = scratch_pte;

		sf_buf_free(sf);
		sched_unpin();

		num_entries -= last_pte - first_pte;
		first_pte = 0;
		act_pd++;
	}

}

int i915_gem_init_aliasing_ppgtt(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct i915_hw_ppgtt *ppgtt;
	unsigned first_pd_entry_in_global_pt;
	int i;


	/* ppgtt PDEs reside in the global gtt pagetable, which has 512*1024
	 * entries. For aliasing ppgtt support we just steal them at the end for
	 * now.  */
	first_pd_entry_in_global_pt = 512 * 1024 - I915_PPGTT_PD_ENTRIES;

	ppgtt = malloc(sizeof(*ppgtt), DRM_I915_GEM, M_WAITOK | M_ZERO);

	ppgtt->num_pd_entries = I915_PPGTT_PD_ENTRIES;
	ppgtt->pt_pages = malloc(sizeof(vm_page_t) * ppgtt->num_pd_entries,
	    DRM_I915_GEM, M_WAITOK | M_ZERO);

	for (i = 0; i < ppgtt->num_pd_entries; i++) {
		ppgtt->pt_pages[i] = vm_page_alloc(NULL, 0,
		    VM_ALLOC_NORMAL | VM_ALLOC_NOOBJ | VM_ALLOC_WIRED |
		    VM_ALLOC_ZERO);
		if (ppgtt->pt_pages[i] == NULL) {
			dev_priv->mm.aliasing_ppgtt = ppgtt;
			i915_gem_cleanup_aliasing_ppgtt(dev);
			return (-ENOMEM);
		}
	}

	ppgtt->scratch_page_dma_addr = dev_priv->mm.gtt.scratch_page_dma;

	i915_ppgtt_clear_range(ppgtt, 0, ppgtt->num_pd_entries *
	    I915_PPGTT_PT_ENTRIES);
	ppgtt->pd_offset = (first_pd_entry_in_global_pt) * sizeof(uint32_t);
	dev_priv->mm.aliasing_ppgtt = ppgtt;

	return 0;
}

static void i915_ppgtt_insert_pages(struct i915_hw_ppgtt *ppgtt,
					 unsigned first_entry,
					 unsigned num_entries,
					 vm_page_t *pages,
					 uint32_t pte_flags)
{
	uint32_t *pt_vaddr, pte;
	unsigned act_pd = first_entry / I915_PPGTT_PT_ENTRIES;
	unsigned first_pte = first_entry % I915_PPGTT_PT_ENTRIES;
	unsigned j, last_pte;
	vm_paddr_t page_addr;
	struct sf_buf *sf;

	while (num_entries) {
		last_pte = first_pte + num_entries;
		if (last_pte > I915_PPGTT_PT_ENTRIES)
			last_pte = I915_PPGTT_PT_ENTRIES;

		sched_pin();
		sf = sf_buf_alloc(ppgtt->pt_pages[act_pd], SFB_CPUPRIVATE);
		pt_vaddr = (uint32_t *)(uintptr_t)sf_buf_kva(sf);

		for (j = first_pte; j < last_pte; j++) {
			page_addr = VM_PAGE_TO_PHYS(*pages);
			pte = GEN6_PTE_ADDR_ENCODE(page_addr);
			pt_vaddr[j] = pte | pte_flags;

			pages++;
		}

		sf_buf_free(sf);
		sched_unpin();

		num_entries -= last_pte - first_pte;
		first_pte = 0;
		act_pd++;
	}
}

void i915_ppgtt_bind_object(struct i915_hw_ppgtt *ppgtt,
			    struct drm_i915_gem_object *obj,
			    enum i915_cache_level cache_level)
{
	struct drm_device *dev;
	struct drm_i915_private *dev_priv;
	uint32_t pte_flags;

	dev = obj->base.dev;
	dev_priv = dev->dev_private;
	pte_flags = GEN6_PTE_VALID;

	switch (cache_level) {
	case I915_CACHE_LLC_MLC:
		pte_flags |= GEN6_PTE_CACHE_LLC_MLC;
		break;
	case I915_CACHE_LLC:
		pte_flags |= GEN6_PTE_CACHE_LLC;
		break;
	case I915_CACHE_NONE:
		pte_flags |= GEN6_PTE_UNCACHED;
		break;
	default:
		panic("cache mode");
	}

	i915_ppgtt_insert_pages(ppgtt, obj->gtt_space->start >> PAGE_SHIFT,
	    obj->base.size >> PAGE_SHIFT, obj->pages, pte_flags);
}

void i915_ppgtt_unbind_object(struct i915_hw_ppgtt *ppgtt,
			      struct drm_i915_gem_object *obj)
{
	i915_ppgtt_clear_range(ppgtt,
			       obj->gtt_space->start >> PAGE_SHIFT,
			       obj->base.size >> PAGE_SHIFT);
}

void i915_gem_init_ppgtt(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	uint32_t pd_offset;
	struct intel_ring_buffer *ring;
	struct i915_hw_ppgtt *ppgtt = dev_priv->mm.aliasing_ppgtt;
	u_int first_pd_entry_in_global_pt;
	uint32_t pd_entry;
	int i;

	if (!dev_priv->mm.aliasing_ppgtt)
		return;


	first_pd_entry_in_global_pt = 512 * 1024 - I915_PPGTT_PD_ENTRIES;
	for (i = 0; i < ppgtt->num_pd_entries; i++) {
		vm_paddr_t pt_addr;

		pt_addr = VM_PAGE_TO_PHYS(ppgtt->pt_pages[i]);
		pd_entry = GEN6_PDE_ADDR_ENCODE(pt_addr);
		pd_entry |= GEN6_PDE_VALID;

		intel_gtt_write(first_pd_entry_in_global_pt + i, pd_entry);
	}
	intel_gtt_read_pte(first_pd_entry_in_global_pt);

	pd_offset = ppgtt->pd_offset;
	pd_offset /= 64; /* in cachelines, */
	pd_offset <<= 16;

	if (INTEL_INFO(dev)->gen == 6) {
		uint32_t ecochk, gab_ctl, ecobits;

		ecobits = I915_READ(GAC_ECO_BITS);
		I915_WRITE(GAC_ECO_BITS, ecobits | ECOBITS_PPGTT_CACHE64B);

		gab_ctl = I915_READ(GAB_CTL);
		I915_WRITE(GAB_CTL, gab_ctl | GAB_CTL_CONT_AFTER_PAGEFAULT);

		ecochk = I915_READ(GAM_ECOCHK);
		I915_WRITE(GAM_ECOCHK, ecochk | ECOCHK_SNB_BIT |
				       ECOCHK_PPGTT_CACHE64B);
		I915_WRITE(GFX_MODE, _MASKED_BIT_ENABLE(GFX_PPGTT_ENABLE));
	} else if (INTEL_INFO(dev)->gen >= 7) {
		I915_WRITE(GAM_ECOCHK, ECOCHK_PPGTT_CACHE64B);
		/* GFX_MODE is per-ring on gen7+ */
	}

	for_each_ring(ring, dev_priv, i) {
		if (INTEL_INFO(dev)->gen >= 7)
			I915_WRITE(RING_MODE_GEN7(ring),
				   _MASKED_BIT_ENABLE(GFX_PPGTT_ENABLE));

		I915_WRITE(RING_PP_DIR_DCLV(ring), PP_DIR_DCLV_2G);
		I915_WRITE(RING_PP_DIR_BASE(ring), pd_offset);
	}
}

static bool do_idling(struct drm_i915_private *dev_priv)
{
	bool ret = dev_priv->mm.interruptible;

	if (dev_priv->mm.gtt.do_idle_maps) {
		dev_priv->mm.interruptible = false;
		if (i915_gpu_idle(dev_priv->dev)) {
			DRM_ERROR("Couldn't idle GPU\n");
			/* Wait a bit, in hopes it avoids the hang */
			DELAY(10);
		}
	}

	return ret;
}

static void undo_idling(struct drm_i915_private *dev_priv, bool interruptible)
{
	if (dev_priv->mm.gtt.do_idle_maps)
		dev_priv->mm.interruptible = interruptible;
}

void
i915_gem_cleanup_aliasing_ppgtt(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv;
	struct i915_hw_ppgtt *ppgtt;
	vm_page_t m;
	int i;

	dev_priv = dev->dev_private;
	ppgtt = dev_priv->mm.aliasing_ppgtt;
	if (ppgtt == NULL)
		return;
	dev_priv->mm.aliasing_ppgtt = NULL;

	for (i = 0; i < ppgtt->num_pd_entries; i++) {
		m = ppgtt->pt_pages[i];
		if (m != NULL) {
			vm_page_unwire(m, PQ_INACTIVE);
			vm_page_free(m);
		}
	}
	free(ppgtt->pt_pages, DRM_I915_GEM);
	free(ppgtt, DRM_I915_GEM);
}


static unsigned int
cache_level_to_agp_type(struct drm_device *dev, enum i915_cache_level
    cache_level)
{

	switch (cache_level) {
	case I915_CACHE_LLC_MLC:
		if (INTEL_INFO(dev)->gen >= 6)
			return (AGP_USER_CACHED_MEMORY_LLC_MLC);
		/*
		 * Older chipsets do not have this extra level of CPU
		 * cacheing, so fallthrough and request the PTE simply
		 * as cached.
		 */
	case I915_CACHE_LLC:
		return (AGP_USER_CACHED_MEMORY);

	default:
	case I915_CACHE_NONE:
		return (AGP_USER_MEMORY);
	}
}

void i915_gem_restore_gtt_mappings(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_gem_object *obj;

	/* First fill our portion of the GTT with scratch pages */
	intel_gtt_clear_range(dev_priv->mm.gtt_start / PAGE_SIZE,
			      (dev_priv->mm.gtt_end - dev_priv->mm.gtt_start) / PAGE_SIZE);

	list_for_each_entry(obj, &dev_priv->mm.gtt_list, gtt_list) {
		i915_gem_clflush_object(obj);
		i915_gem_gtt_bind_object(obj, obj->cache_level);
	}

	intel_gtt_chipset_flush();
}

int i915_gem_gtt_prepare_object(struct drm_i915_gem_object *obj)
{

	return 0;
}

void i915_gem_gtt_bind_object(struct drm_i915_gem_object *obj,
			      enum i915_cache_level cache_level)
{
	struct drm_device *dev;
	struct drm_i915_private *dev_priv;
	unsigned int agp_type;

	dev = obj->base.dev;
	dev_priv = dev->dev_private;
	agp_type = cache_level_to_agp_type(dev, cache_level);

	intel_gtt_insert_pages(obj->gtt_space->start >> PAGE_SHIFT,
	    obj->base.size >> PAGE_SHIFT, obj->pages, agp_type);

	obj->has_global_gtt_mapping = 1;
}

void i915_gem_gtt_unbind_object(struct drm_i915_gem_object *obj)
{

	intel_gtt_clear_range(obj->gtt_space->start >> PAGE_SHIFT,
	    obj->base.size >> PAGE_SHIFT);

	obj->has_global_gtt_mapping = 0;
}

void i915_gem_gtt_finish_object(struct drm_i915_gem_object *obj)
{
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	bool interruptible;

	interruptible = do_idling(dev_priv);

	undo_idling(dev_priv, interruptible);
}

int i915_gem_init_global_gtt(struct drm_device *dev,
			      unsigned long start,
			      unsigned long mappable_end,
			      unsigned long end)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	unsigned long mappable;
	int error;

	mappable = min(end, mappable_end) - start;

	/* Substract the guard page ... */
	drm_mm_init(&dev_priv->mm.gtt_space, start, end - start - PAGE_SIZE);

	dev_priv->mm.gtt_start = start;
	dev_priv->mm.gtt_mappable_end = mappable_end;
	dev_priv->mm.gtt_end = end;
	dev_priv->mm.gtt_total = end - start;
	dev_priv->mm.mappable_gtt_total = mappable;

	/* ... but ensure that we clear the entire range. */
	intel_gtt_clear_range(start / PAGE_SIZE, (end-start) / PAGE_SIZE);
	device_printf(dev->dev,
	    "taking over the fictitious range 0x%lx-0x%lx\n",
	    dev->agp->base + start, dev->agp->base + start + mappable);
	error = -vm_phys_fictitious_reg_range(dev->agp->base + start,
	    dev->agp->base + start + mappable, VM_MEMATTR_WRITE_COMBINING);
	return (error);
}
