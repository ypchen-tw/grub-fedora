/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2006  Free Software Foundation, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301, USA.
 */

#include <config.h>
#include <grub/misc.h>
#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#include <grub/efi/misc.h>

#include <shared.h>

#define NEXT_MEMORY_DESCRIPTOR(desc, size)	\
  ((grub_efi_memory_descriptor_t *) ((char *) (desc) + (size)))

#define BYTES_TO_PAGES(bytes)	((bytes) >> 12)
#define PAGES_TO_BYTES(pages)	((pages) << 12)

/* The size of a memory map obtained from the firmware. This must be
   a multiplier of 4KB.  */
#define MEMORY_MAP_SIZE	0x2000

/* Maintain the list of allocated pages.  */
struct allocated_page
{
  grub_efi_physical_address_t addr;
  grub_efi_uint64_t num_pages;
};

#define ALLOCATED_PAGES_SIZE	0x1000
#define MAX_ALLOCATED_PAGES	\
  (ALLOCATED_PAGES_SIZE / sizeof (struct allocated_page))

static struct allocated_page *allocated_pages = 0;

/* The minimum and maximum heap size for GRUB itself.  */
#define MIN_HEAP_SIZE	0x100000
#define MAX_HEAP_SIZE	(16 * 0x100000)


void *
grub_efi_allocate_pool (grub_efi_uintn_t size)
{
  grub_efi_status_t status;
  void *p;
  grub_efi_boot_services_t *b;

  b = grub_efi_system_table->boot_services;
  status = Call_Service_3(b->allocate_pool, GRUB_EFI_LOADER_DATA, size, &p);
  if (status != GRUB_EFI_SUCCESS)
    return NULL;

  return p;
}

void
grub_efi_free_pool (void *buffer)
{
  grub_efi_boot_services_t *b;

  b = grub_efi_system_table->boot_services;
  Call_Service_1(b->free_pool, buffer);
}

void *
grub_efi_allocate_anypages(grub_efi_uintn_t pages)
{
  grub_efi_boot_services_t *b;
  grub_efi_status_t status;
  grub_efi_physical_address_t address;

  b = grub_efi_system_table->boot_services;
  status = Call_Service_4 (b->allocate_pages,
			    GRUB_EFI_ALLOCATE_ANY_PAGES,
			    GRUB_EFI_LOADER_DATA,
			    pages,
			    &address);
  if (status != GRUB_EFI_SUCCESS)
  	return 0;

  if (allocated_pages)
     {
       unsigned i;
 
       for (i = 0; i < MAX_ALLOCATED_PAGES; i++)
 	if (allocated_pages[i].addr == 0)
        {
              allocated_pages[i].addr = address;
              allocated_pages[i].num_pages = pages;
              break;
        }
 
       if (i == MAX_ALLOCATED_PAGES)
        {
           grub_printf ("too many page allocations");
           return NULL;
        }
     }
 
  return (void *) ((grub_addr_t) address);

}

/* Allocate pages. Return the pointer to the first of allocated pages.  */
void *
grub_efi_allocate_pages (grub_efi_physical_address_t address,
			 grub_efi_uintn_t pages)
{
  grub_efi_allocate_type_t type;
  grub_efi_status_t status;
  grub_efi_boot_services_t *b;

  /* Limit the memory access to less than 2GB to avoid 64bit
   * compatible problem of grub  */
  if (address > 0x7fffffff)
    return 0;

  if (address == 0)
    {
      type = GRUB_EFI_ALLOCATE_MAX_ADDRESS;
      address = 0x7fffffff;
    }
  else
    type = GRUB_EFI_ALLOCATE_ADDRESS;

  b = grub_efi_system_table->boot_services;
  status = Call_Service_4 (b->allocate_pages, type,
			   GRUB_EFI_LOADER_DATA, pages, &address);
  if (status != GRUB_EFI_SUCCESS)
    return 0;

  if (address == 0)
    {
      /* Uggh, the address 0 was allocated... This is too annoying,
	 so reallocate another one.  */
      address = 0x7fffffff;
      status = Call_Service_4 (b->allocate_pages,
				type, GRUB_EFI_LOADER_DATA, pages, &address);
      grub_efi_free_pages (0, pages);
      if (status != GRUB_EFI_SUCCESS)
	return 0;
    }

  if (allocated_pages)
    {
      unsigned i;

      for (i = 0; i < MAX_ALLOCATED_PAGES; i++)
	if (allocated_pages[i].addr == 0)
	  {
	    allocated_pages[i].addr = address;
	    allocated_pages[i].num_pages = pages;
	    break;
	  }

      if (i == MAX_ALLOCATED_PAGES)
	{
	  grub_printf ("too many page allocations");
	  return NULL;
	}
    }

  return (void *) ((grub_addr_t) address);
}

/* Free pages starting from ADDRESS.  */
void
grub_efi_free_pages (grub_efi_physical_address_t address,
		     grub_efi_uintn_t pages)
{
  grub_efi_boot_services_t *b;

  if (allocated_pages
      && ((grub_efi_physical_address_t) ((grub_addr_t) allocated_pages)
	  != address))
    {
      unsigned i;

      for (i = 0; i < MAX_ALLOCATED_PAGES; i++)
	if (allocated_pages[i].addr == address)
	  {
	    allocated_pages[i].addr = 0;
	    break;
	  }
    }

  b = grub_efi_system_table->boot_services;
  Call_Service_2 (b->free_pages ,address, pages);
}

/* Get the memory map as defined in the EFI spec. Return 1 if successful,
   return 0 if partial, or return -1 if an error occurs.  */
int
grub_efi_get_memory_map (grub_efi_uintn_t *memory_map_size,
			 grub_efi_memory_descriptor_t *memory_map,
			 grub_efi_uintn_t *map_key,
			 grub_efi_uintn_t *descriptor_size,
			 grub_efi_uint32_t *descriptor_version)
{
  grub_efi_status_t status;
  grub_efi_boot_services_t *b;
  grub_efi_uintn_t key;
  grub_efi_uint32_t version;

  /* Allow some parameters to be missing.  */
  if (! map_key)
    map_key = &key;
  if (! descriptor_version)
    descriptor_version = &version;

  b = grub_efi_system_table->boot_services;
  status = Call_Service_5 (b->get_memory_map,
			      memory_map_size, memory_map, map_key,
			      descriptor_size, descriptor_version);
  if (status == GRUB_EFI_SUCCESS)
    return 1;
  else if (status == GRUB_EFI_BUFFER_TOO_SMALL)
    return 0;
  else
    return -1;
}

#define MMAR_DESC_LENGTH	20

/*
 * Add a memory region to the kernel e820 map.
 */
static void
add_memory_region (struct e820_entry *e820_map,
		   int *e820_nr_map,
		   unsigned long long start,
		   unsigned long long size,
		   unsigned int type)
{
  int x = *e820_nr_map;

  if (x == E820_MAX)
    {
      grub_printf ("Too many entries in the memory map!\n");
      return;
    }

  if (e820_map[x-1].addr + e820_map[x-1].size == start
      && e820_map[x-1].type == type)
    {
      e820_map[x-1].size += size;
    }
  else
    {
      e820_map[x].addr = start;
      e820_map[x].size = size;
      e820_map[x].type = type;
      (*e820_nr_map)++;
    }
}

/*
 * Make a e820 memory map
 */
void
e820_map_from_efi_map (struct e820_entry *e820_map,
		       int *e820_nr_map,
		       grub_efi_memory_descriptor_t *memory_map,
		       grub_efi_uintn_t desc_size,
		       grub_efi_uintn_t memory_map_size)
{
  grub_efi_memory_descriptor_t *desc;
  unsigned long long start = 0;
  unsigned long long end = 0;
  unsigned long long size = 0;
  grub_efi_memory_descriptor_t *memory_map_end;

  memory_map_end = NEXT_MEMORY_DESCRIPTOR (memory_map, memory_map_size);
  *e820_nr_map = 0;
  for (desc = memory_map;
       desc < memory_map_end;
       desc = NEXT_MEMORY_DESCRIPTOR (desc, desc_size))
    {
      switch (desc->type)
	{
	case GRUB_EFI_ACPI_RECLAIM_MEMORY:
	  add_memory_region (e820_map, e820_nr_map,
			     desc->physical_start, desc->num_pages << 12,
			     E820_ACPI);
	  break;
	case GRUB_EFI_RUNTIME_SERVICES_CODE:
	case GRUB_EFI_RUNTIME_SERVICES_DATA:
	case GRUB_EFI_RESERVED_MEMORY_TYPE:
	case GRUB_EFI_MEMORY_MAPPED_IO:
	case GRUB_EFI_MEMORY_MAPPED_IO_PORT_SPACE:
	case GRUB_EFI_UNUSABLE_MEMORY:
	case GRUB_EFI_PAL_CODE:
	  add_memory_region (e820_map, e820_nr_map,
			     desc->physical_start, desc->num_pages << 12,
			     E820_RESERVED);
	  break;
	case GRUB_EFI_LOADER_CODE:
	case GRUB_EFI_LOADER_DATA:
	case GRUB_EFI_BOOT_SERVICES_CODE:
	case GRUB_EFI_BOOT_SERVICES_DATA:
	case GRUB_EFI_CONVENTIONAL_MEMORY:
	  start = desc->physical_start;
	  size = desc->num_pages << 12;
	  end = start + size;
	  if (start < 0x100000ULL && end > 0xA0000ULL)
	    {
	      if (start < 0xA0000ULL)
		add_memory_region (e820_map, e820_nr_map,
				   start, 0xA0000ULL-start,
				   E820_RAM);
	      if (end <= 0x100000ULL)
		continue;
	      start = 0x100000ULL;
	      size = end - start;
	    }
	  add_memory_region (e820_map, e820_nr_map,
			     start, size, E820_RAM);
	  break;
	case GRUB_EFI_ACPI_MEMORY_NVS:
	  add_memory_region (e820_map, e820_nr_map,
			     desc->physical_start, desc->num_pages << 12,
			     E820_NVS);
	  break;
	}
    }
}

static void
update_e820_map (struct e820_entry *e820_map,
		 int *e820_nr_map)
{
  grub_efi_memory_descriptor_t *memory_map;
  grub_efi_uintn_t map_size;
  grub_efi_uintn_t desc_size;

  /* Prepare a memory region to store memory map.  */
  memory_map = grub_efi_allocate_pages (0, BYTES_TO_PAGES (MEMORY_MAP_SIZE));
  if (! memory_map)
    {
      grub_printf ("cannot allocate memory");
      return;
    }

  /* Obtain descriptors for available memory.  */
  map_size = MEMORY_MAP_SIZE;

  if (grub_efi_get_memory_map (&map_size, memory_map, 0, &desc_size, 0) < 0)
    {
      grub_printf ("cannot get memory map");
      return;
    }

  e820_map_from_efi_map (e820_map, e820_nr_map,
			 memory_map, desc_size, map_size);

  /* Release the memory map.  */
  grub_efi_free_pages ((grub_addr_t) memory_map,
		       BYTES_TO_PAGES (MEMORY_MAP_SIZE));
}

/* Simulated memory sizes. */
#define EXTENDED_MEMSIZE (3 * 1024 * 1024)	/* 3MB */
#define CONVENTIONAL_MEMSIZE (640 * 1024)	/* 640kB */

int
get_code_end (void)
{
  /* Just return a little area for simulation. */
  return BOOTSEC_LOCATION + (60 * 1024);
}

/* memory probe routines */
int
get_memsize (int type)
{
  if (! type)
    return CONVENTIONAL_MEMSIZE >> 10;
  else
    return EXTENDED_MEMSIZE >> 10;
}

/* get_eisamemsize() :  return packed EISA memory map, lower 16 bits is
 *		memory between 1M and 16M in 1K parts, upper 16 bits is
 *		memory above 16M in 64K parts.  If error, return -1.
 */
int
get_eisamemsize (void)
{
  return (EXTENDED_MEMSIZE >> 10);
}

static int grub_e820_nr_map;
static struct e820_entry grub_e820_map[E820_MAX];

/* Fetch the next entry in the memory map and return the continuation
   value.  DESC is a pointer to the descriptor buffer, and CONT is the
   previous continuation value (0 to get the first entry in the
   map).  */
int
get_mmap_entry (struct mmar_desc *desc, int cont)
{
  if (cont < 0 || cont >= grub_e820_nr_map)
    {
      /* Should not happen.  */
      desc->desc_len = 0;
    }
  else
    {
      struct e820_entry *entry;
      /* Copy the entry.  */
      desc->desc_len = MMAR_DESC_LENGTH;
      entry = &grub_e820_map[cont++];
      desc->addr = entry->addr;
      desc->length = entry->size;
      desc->type = entry->type;

      /* If the next entry exists, return the index.  */
      if (cont < grub_e820_nr_map)
	return cont;
    }

  return 0;
}

void
grub_efi_mm_init (void)
{
  /* First of all, allocate pages to maintain allocations.  */
  allocated_pages
    = grub_efi_allocate_pages (0, BYTES_TO_PAGES (ALLOCATED_PAGES_SIZE));
  if (! allocated_pages)
    {
      grub_printf ("cannot allocate memory");
      return;
    }

  grub_memset (allocated_pages, 0, ALLOCATED_PAGES_SIZE);

  update_e820_map (grub_e820_map, &grub_e820_nr_map);
}

void
grub_efi_mm_fini (void)
{
  if (allocated_pages)
    {
      unsigned i;

      for (i = 0; i < MAX_ALLOCATED_PAGES; i++)
	{
	  struct allocated_page *p;

	  p = allocated_pages + i;
	  if (p->addr != 0)
	    grub_efi_free_pages ((grub_addr_t) p->addr, p->num_pages);
	}

      grub_efi_free_pages ((grub_addr_t) allocated_pages,
			   BYTES_TO_PAGES (ALLOCATED_PAGES_SIZE));
    }
}
