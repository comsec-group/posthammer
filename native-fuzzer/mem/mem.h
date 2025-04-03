enum mem_hp_type { TWO_MB, ONE_GB };

char *mem_get_hp(enum mem_hp_type type);
unsigned long virt_to_pfn(unsigned long virt);
