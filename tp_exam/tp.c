#include <debug.h>
#include <segmem.h>
#include <string.h>
#include <pagemem.h>
#include <cr.h>
#include <intr.h>
#include <pic.h>
#include <io.h>


/*  Structure d'un processus*/
struct process {
	unsigned int pid;

	struct {
		uint32_t eax, ecx, edx, ebx;
		uint32_t esp, ebp, esi, edi;
		uint32_t eip, eflags;
		uint16_t cs, ss;
		uint32_t cr3;
	} regs __attribute__ ((packed));
} __attribute__ ((packed));


/*Liste des processus, nombre de processus et processus en cours*/
struct process p_list[2];
struct process *current = 0;
unsigned int n_proc = 0;

/*Creation d'une GDT et d'un TSS*/
seg_desc_t GDT[7];
tss_t TSS;

/*Une PGD pour chaque processus*/
pde32_t *pgd1;
pde32_t *pgd2;


// ---------------------------------------------------- GDT et TSS ----------------------------------------------------
#define c0_idx  1
#define d0_idx  2
#define c3_idx  3
#define d3_idx  4
#define ts_idx  5
#define ts_idxB  6

#define c0_sel  gdt_krn_seg_sel(c0_idx)
#define d0_sel  gdt_krn_seg_sel(d0_idx)
#define c3_sel  gdt_usr_seg_sel(c3_idx)
#define d3_sel  gdt_usr_seg_sel(d3_idx)
#define ts_sel  gdt_krn_seg_sel(ts_idx)


#define gdt_flat_dsc(_dSc_,_pVl_,_tYp_) ({      \
    (_dSc_)->raw = 0;                           \
    (_dSc_)->limit_1 = 0xFFFF;                  \
    (_dSc_)->limit_2 = 0xF;                     \
    (_dSc_)->type = _tYp_;                      \
    (_dSc_)->dpl = _pVl_;                       \
    (_dSc_)->d = 1;                             \
    (_dSc_)->g = 1;                             \
    (_dSc_)->s = 1;                             \
    (_dSc_)->p = 1;                             \
})

#define tss_dsc(_dSc_,_tSs_) ({                 \
    raw32_t addr = {.raw = _tSs_};              \
    (_dSc_)->raw = sizeof(tss_t);               \
    (_dSc_)->base_1 = addr.wlow;                \
    (_dSc_)->base_2 = addr._whigh.blow;         \
    (_dSc_)->base_3 = addr._whigh.bhigh;        \
    (_dSc_)->type = SEG_DESC_SYS_TSS_AVL_32;    \
    (_dSc_)->p = 1;                             \
})

#define c0_dsc(_d) gdt_flat_dsc(_d, 0, SEG_DESC_CODE_XR)
#define d0_dsc(_d) gdt_flat_dsc(_d, 0, SEG_DESC_DATA_RW)
#define c3_dsc(_d) gdt_flat_dsc(_d, 3, SEG_DESC_CODE_XR)
#define d3_dsc(_d) gdt_flat_dsc(_d, 3, SEG_DESC_DATA_RW)

void init_gdt() {
    gdt_reg_t gdtr;

    GDT[0].raw = 0ULL;
    c0_dsc(&GDT[c0_idx]);
    d0_dsc(&GDT[d0_idx]);
    c3_dsc(&GDT[c3_idx]);
    d3_dsc(&GDT[d3_idx]);

    gdtr.desc = GDT;
    gdtr.limit = sizeof(GDT) - 1;
    set_gdtr(gdtr);

    set_cs(c0_sel);
    set_ss(d0_sel);
    set_ds(d0_sel);
    set_es(d0_sel);
    set_fs(d0_sel);
    set_gs(d0_sel);
}

// ---------------------------------------------------- Interruption et Appel Système ----------------------------------------------------

void syscall_isr() {
   asm volatile (
      "leave ; pusha        \n"
      "push %eax     \n"
      "call syscall_handler \n"
	  "pop %eax           \n"
      "popa ; iret"
      );
}

void __regparm__(1) syscall_handler(int sys_num) {

	uint32_t *counter;

	  if (sys_num ==1){
      	asm volatile("mov  %%ebx, %0":"=m"(counter) :);
   	  	debug("Valeur compteur: %d\n", *counter);
	  } else {
		debug("Erreur syscall inexistant");
	  }
}

void schedule(void){
   	
	uint32_t * stack_ptr;
	uint32_t ss,cs;
   uint32_t esp0;

	asm("mov  %%ebp, %%eax;mov  %%eax, %0":"=m"(stack_ptr) :);

	debug("%c", 0);
   
   //Sauvegarde du contexte 
   current->regs.edi = stack_ptr[2];
   current->regs.esi = stack_ptr[3];
   current->regs.ebp = stack_ptr[10];
   current->regs.ebx = stack_ptr[6];
   current->regs.edx = stack_ptr[7];
   current->regs.ecx = stack_ptr[8];
   current->regs.eax = stack_ptr[9];
   current->regs.eip = stack_ptr[11];
   current->regs.cs = stack_ptr[12];
   current->regs.eflags = stack_ptr[13];
   current->regs.esp = stack_ptr[14];
   current->regs.ss = stack_ptr[15];

   //Nettoyage de la pile 
	TSS.s0.esp = (uint32_t) (stack_ptr +16);
	esp0 = TSS.s0.esp;

   //Changement du processus courant 
	if (n_proc > current->pid+1){
		current = &p_list[current->pid+1];
   } else {
		current = &p_list[0];
   }
	
   ss = (uint16_t)current->regs.ss;
   cs = (uint16_t)current->regs.cs;


   //Création de la pile du nouveau processus
	asm volatile (
		"mov %0, %%esp\n"
      "push %1      \n"
		"push %2      \n"
		"push %3      \n"
		"push %4      \n"
		"push %5      \n"
		::
		"r"(esp0),
      "r"(ss),
      "r"(current->regs.esp),
      "r"(current->regs.eflags),
		"r"(cs),
		"r"(current->regs.eip)
	);

	asm volatile (
      "push %0      \n"
		"push %1      \n"
		"push %2      \n"
		"push %3      \n"
		"push %4      \n"
		"push %5      \n"
		::
		"r"(current->regs.ebp),
      "r"(current->regs.eax),
      "r"(current->regs.ecx),
      "r"(current->regs.edx),
		"r"(current->regs.ebx),
		"r"(current->regs.esp) 
	);

	asm volatile (
      "push %0      \n"
		"push %1      \n"
		"push %2      \n"
      "mov %3, %%eax  \n"
      "mov %%eax, %%cr3  \n"
		::
		"r"(current->regs.ebp),
      "r"(current->regs.esi),
      "r"(current->regs.edi),
      "r"(current->regs.cr3)
	);

}

void irq0_handler() {
	asm volatile (
		"pusha       \n"
		"call schedule \n"
		"popa       \n"
		"add $4, %esp   \n"
		"iret       \n"
	);
}

void sys_counter(uint32_t * counter){
      asm volatile("mov  %0, %%ebx;mov $0x01, %%eax":"=m"(counter) :);
      asm volatile ("int $0x80");
}

//-----------------------------------------------------Fonction compteurs (Ecriture et Lecture) ----------------------------


//Incrémentation du compteur 
__attribute__((section(".user1.text"))) void user1() {
	
	uint32_t *counter = (uint32_t *)0x706000; // Adresse virtuelle dans la zone partagée
    while (1) {
        // Incrémente le compteur
      (*counter)++;
		for(int i = 0; i<50000000; i++);
    }
}

//Affichage du compteur
__attribute__((section(".user2.text")))  void user2() {

    uint32_t *counter = (uint32_t *)0x806000; // Adresse virtuelle dans la zone partagée

    while (1) {
      sys_counter(counter);
		for(int i = 0; i<40000000; i++);
    }
}

//----------------------------------------------------Initialisation des tables de pages ----------------------------------------

void init_tables(){

//---------------------------------------------------------Process 1 -----------------------------------------------------------
	pgd1 = (pde32_t*)0x700000;
	pte32_t *ptb1 = (pte32_t*)0x701000;
	pte32_t *ptb_K = (pte32_t*)0x702000;

	for(int i=0;i<1024;i++) {
	 	pg_set_entry(&pgd1[i], PG_USR|PG_RW, i);
		pg_set_entry(&ptb1[i], PG_USR|PG_RW, i);
		pg_set_entry(&ptb_K[i], PG_USR|PG_RW, i);
	}


	pg_set_entry(&pgd1[pd32_get_idx(0x700000)], PG_USR|PG_RW, page_get_nr(ptb1));
	pg_set_entry(&pgd1[pd32_get_idx(0x900000)], PG_USR|PG_RW, page_get_nr(ptb1));

	pg_set_entry(&ptb1[pt32_get_idx(0x704000)], PG_USR|PG_RW, page_get_nr(0x704000));
	pg_set_entry(&ptb1[pt32_get_idx(0x706000)], PG_USR|PG_RW, page_get_nr(0x706000));
	pg_set_entry(&ptb1[pt32_get_idx(0x900000)], PG_USR|PG_RW, page_get_nr(0x900000));
	pg_set_entry(&ptb1[pt32_get_idx(0x902000)], PG_USR|PG_RW, page_get_nr(0x902000));
	pg_set_entry(&ptb1[pt32_get_idx(0x804000)], PG_USR|PG_RW, page_get_nr(0x804000));
	pg_set_entry(&ptb1[pt32_get_idx(0x806000)], PG_USR|PG_RW, page_get_nr(0x706000));


	pg_set_entry(&pgd1[pd32_get_idx(0x300000)], PG_USR|PG_RW, page_get_nr(ptb_K));
	pg_set_entry(&ptb_K[pt32_get_idx(0x300000)], PG_KRN|PG_RW, page_get_nr(0x300000));
	pg_set_entry(&ptb_K[pt32_get_idx(0x301000)], PG_KRN|PG_RW, page_get_nr(0x301000));
	pg_set_entry(&ptb_K[pt32_get_idx(0x302000)], PG_KRN|PG_RW, page_get_nr(0x302000));
	pg_set_entry(&ptb_K[pt32_get_idx(0x303000)], PG_KRN|PG_RW, page_get_nr(0x303000));
	pg_set_entry(&ptb1[pt32_get_idx(0x400000)], PG_KRN|PG_RW, page_get_nr(0x400000));


	//---------------------------------------------------------Process 2 -----------------------------------------------------------
	
	pgd2 = (pde32_t*)0x800000;
	pte32_t *ptb2 = (pte32_t*)0x801000;
	pte32_t *ptb_K2 = (pte32_t*)0x802000;

	for(int i=0;i<1024;i++) {
	 	pg_set_entry(&pgd2[i], PG_USR|PG_RW, i);
		pg_set_entry(&ptb2[i], PG_USR|PG_RW, i);
		pg_set_entry(&ptb_K2[i], PG_USR|PG_RW, i);
	}

	pg_set_entry(&pgd2[pd32_get_idx(0x800000)], PG_USR|PG_RW, page_get_nr(ptb2));
	pg_set_entry(&ptb2[pt32_get_idx(0x804000)], PG_USR|PG_RW, page_get_nr(0x804000));
	pg_set_entry(&ptb2[pt32_get_idx(0x806000)], PG_USR|PG_RW, page_get_nr(0x706000));
	pg_set_entry(&ptb2[pt32_get_idx(0x902000)], PG_USR|PG_RW, page_get_nr(0x902000));

	pg_set_entry(&pgd2[pd32_get_idx(0x300000)], PG_USR|PG_RW, page_get_nr(ptb_K2));
   pg_set_entry(&pgd2[pd32_get_idx(0x400000)], PG_USR|PG_RW, page_get_nr(ptb2));

   pg_set_entry(&ptb_K2[pt32_get_idx(0x300000)], PG_USR|PG_RW, page_get_nr(0x300000));
   pg_set_entry(&ptb_K2[pt32_get_idx(0x301000)], PG_KRN|PG_RW, page_get_nr(0x301000));
   pg_set_entry(&ptb_K2[pt32_get_idx(0x302000)], PG_KRN|PG_RW, page_get_nr(0x302000));
   pg_set_entry(&ptb_K2[pt32_get_idx(0x303000)], PG_KRN|PG_RW, page_get_nr(0x303000));
	pg_set_entry(&ptb2[pt32_get_idx(0x400000)], PG_KRN|PG_RW, page_get_nr(0x400000)); //Normalement processus 2 ne devrais pas avoir accès à cette pile 
   pg_set_entry(&ptb2[pt32_get_idx(0x402000)], PG_KRN|PG_RW, page_get_nr(0x402000));

}

//--------------------------------------------Initialisation de l'IDTR -------------------------------------------------------
void init_idtr(){

   idt_reg_t idtr;
   get_idtr(idtr);
	int_desc_t *irq0_dsc = &idtr.desc[32];
   irq0_dsc->offset_1 = (uint16_t)((uint32_t)irq0_handler);
   irq0_dsc->offset_2 = (uint16_t)(((uint32_t)irq0_handler)>>16);

   int_desc_t *dsc = &idtr.desc[0x80];
   dsc->offset_1 = (uint16_t)((uint32_t)syscall_isr); // 3 install kernel syscall handler
   dsc->offset_2 = (uint16_t)(((uint32_t)syscall_isr)>>16);
   dsc->dpl = 3;

}

//-----------------------------------------Chargement d'un processus----------------------------------------

void ChargementTache(uint32_t pgd, uint32_t esp, uint32_t fonction){
   
   p_list[n_proc].pid = n_proc;
	p_list[n_proc].regs.cr3 = pgd;
	p_list[n_proc].regs.ss = d3_sel;
	p_list[n_proc].regs.cs = c3_sel;
	p_list[n_proc].regs.esp = esp;
	p_list[n_proc].regs.eip = fonction;
   p_list[n_proc].regs.eflags = 0x200;

	n_proc++;

}

//---------------------------------------Point d'entrée du programme-------------------------------------
void tp() {

   debug("Initialisation de la GDT\n");
   init_gdt();
   
   debug("Initialisation des tables de pages\n");
	init_tables();

   debug("Initialisation de l'IDTR\n");
   init_idtr();

   debug("Chargement des deux processus\n");
   ChargementTache((uint32_t) pgd1, 0x901000, (uint32_t) &user1);
   ChargementTache((uint32_t) pgd2, 0x903000, (uint32_t) &user2);
	
   debug("Mise à 0 du compteur\n");
	*(volatile int*)0x706000 = 0;

   debug("Chargement segment utilisateurs et processus courant\n");
   set_ds(d3_sel);
   set_es(d3_sel);
   set_fs(d3_sel);
   set_gs(d3_sel);
   TSS.s0.esp = 0x401000;
   TSS.s0.ss  = d0_sel;
   tss_dsc(&GDT[ts_idx], (offset_t)&TSS);

	set_tr(ts_sel);

   current = &p_list[0];

    debug("Activiation de la pagination\n");
   set_cr3((uint32_t)pgd1);
	uint32_t cr0 = get_cr0(); // enable paging
	set_cr0(cr0|CR0_PG);

   debug("Activation des interruptions\n");
   asm volatile("sti");

   debug("Passage en mode user et saut dans user1\n");
   asm volatile (
      "push %0 \n" // ss
      "push %1 \n" // esp pour du ring 3 !
      "pushf   \n" // eflags
      "push %2 \n" // cs
      "push %3 \n" // eip
      "iret"
      ::
      "m"(current->regs.ss),
      "m"(current->regs.esp),
      "m"(current->regs.cs),
      "m"(current->regs.eip)
);
	
}
