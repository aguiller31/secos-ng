/**
 * @file tp.c
 * @brief Implémentation d'un système de gestion de processus avec pagination et changement de contexte
 * 
 * Ce fichier implémente un système simple de gestion de processus avec:
 * - Gestion de la mémoire paginée
 * - Changement de contexte entre processus
 * - Gestion des interruptions
 * - Communication inter-processus via mémoire partagée
 */
#include <debug.h>
#include <segmem.h>
#include <string.h>
#include <pagemem.h>
#include <cr.h>
#include <intr.h>
#include <pic.h>
#include <io.h>

/**
 * @struct process
 * @brief Structure représentant un processus
 * 
 * @param pid Identifiant unique du processus
 * @param regs Structure imbriquée contenant l'état des registres du processus
 */
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

/**
 * @var p_list
 * @brief Liste des processus du système
 * Tableau statique contenant les processus du système
 */
struct process p_list[2];

/**
 * @var current
 * @brief Pointeur vers le processus actuellement en cours d'exécution
 */
struct process *current = 0;

/**
 * @var n_proc
 * @brief Nombre total de processus dans le système
 */
unsigned int n_proc = 0;

/**
 * @var GDT
 * @brief Global Descriptor Table du système
 * Table contenant les descripteurs de segments
 */
seg_desc_t GDT[7];

/**
 * @var TSS
 * @brief Task State Segment
 * Structure contenant l'état de la tâche courante
 */
tss_t TSS;

/*Une PGD pour chaque processus*/

/**
@var pgd1
@brief Page Directory du premier processus
Pointeur vers la table des pages du processus 1
*/
pde32_t *pgd1;

/**
@var pgd2
@brief Page Directory du deuxième processus
Pointeur vers la table des pages du processus 2
*/
pde32_t *pgd2;


// ---------------------------------------------------- GDT et TSS ----------------------------------------------------
/**
@def c0_idx
@brief Index du descripteur de segment de code ring 0 dans la GDT
*/
#define c0_idx  1

/**
@def d0_idx
@brief Index du descripteur de segment de données ring 0 dans la GDT
*/
#define d0_idx  2

/**
@def c3_idx
@brief Index du descripteur de segment de code ring 3 dans la GDT
*/
#define c3_idx  3

/**
@def d3_idx
@brief Index du descripteur de segment de données ring 3 dans la GDT
*/
#define d3_idx  4

/**
@def ts_idx
@brief Index du descripteur TSS dans la GDT
*/
#define ts_idx  5

/**
@def ts_idxB
@brief Index du descripteur TSS backup dans la GDT
*/
#define ts_idxB 6

/**
@def c0_sel
@brief Sélecteur de segment de code ring 0
*/
#define c0_sel  gdt_krn_seg_sel(c0_idx)

/**
@def d0_sel
@brief Sélecteur de segment de données ring 0
*/
#define d0_sel  gdt_krn_seg_sel(d0_idx)

/**
@def c3_sel
@brief Sélecteur de segment de code ring 3
*/
#define c3_sel  gdt_usr_seg_sel(c3_idx)

/**
@def d3_sel
@brief Sélecteur de segment de données ring 3
*/
#define d3_sel  gdt_usr_seg_sel(d3_idx)

/**
@def ts_sel
@brief Sélecteur de segment TSS
*/
#define ts_sel  gdt_krn_seg_sel(ts_idx)

/**
@def gdt_flat_dsc(dSc,pVl,tYp)
@brief Macro de création d'un descripteur de segment "flat"
@param dSc Pointeur vers le descripteur à initialiser
@param pVl Niveau de privilège (0 pour kernel, 3 pour user)
@param tYp Type de segment (code ou données)

Initialise un descripteur de segment avec:
* Limite maximale (4GB)
* Base à 0
* Granularité en pages
* Segment 32 bits
*/
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

/**
@def tss_dsc(dSc,tSs)
@brief Macro de création d'un descripteur TSS
@param dSc Pointeur vers le descripteur à initialiser
@param tSs Adresse de la structure TSS

Initialise un descripteur TSS avec:
* Base pointant vers la structure TSS
* Limite égale à la taille de la structure TSS
* Type TSS 32 bits disponible
*/
#define tss_dsc(_dSc_,_tSs_) ({                 \
    raw32_t addr = {.raw = _tSs_};              \
    (_dSc_)->raw = sizeof(tss_t);               \
    (_dSc_)->base_1 = addr.wlow;                \
    (_dSc_)->base_2 = addr._whigh.blow;         \
    (_dSc_)->base_3 = addr._whigh.bhigh;        \
    (_dSc_)->type = SEG_DESC_SYS_TSS_AVL_32;    \
    (_dSc_)->p = 1;                             \
})

/**
@def c0_dsc(_d)
@brief Macro de création d'un descripteur de segment de code ring 0
@param _d Pointeur vers le descripteur
*/
#define c0_dsc(_d) gdt_flat_dsc(_d, 0, SEG_DESC_CODE_XR)

/**
@def d0_dsc(_d)
@brief Macro de création d'un descripteur de segment de données ring 0
@param _d Pointeur vers le descripteur
*/
#define d0_dsc(_d) gdt_flat_dsc(_d, 0, SEG_DESC_DATA_RW)

/**
@def c3_dsc(_d)
@brief Macro de création d'un descripteur de segment de code ring 3
@param _d Pointeur vers le descripteur
*/
#define c3_dsc(_d) gdt_flat_dsc(_d, 3, SEG_DESC_CODE_XR)

/**
@def d3_dsc(_d)
@brief Macro de création d'un descripteur de segment de données ring 3
@param _d Pointeur vers le descripteur
*/
#define d3_dsc(_d) gdt_flat_dsc(_d, 3, SEG_DESC_DATA_RW)

/**
 * @fn void init_gdt()
 * @brief Initialise la GDT et les registres de segments
 * 
 * Configure la GDT avec:
 * - Segments code et données pour ring 0
 * - Segments code et données pour ring 3
 * - Descripteur TSS
 */
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
/**
 * @fn void syscall_isr()
 * @brief Gestionnaire d'interruption pour les appels système
 * 
 * Routine assembleur qui sauvegarde le contexte et appelle syscall_handler
 */
void syscall_isr() {
   asm volatile (
      "leave ; pusha        \n"
      "push %eax     \n"
      "call syscall_handler \n"
	  "pop %eax           \n"
      "popa ; iret"
      );
}

/**
 * @fn void syscall_handler(int sys_num)
 * @brief Gestionnaire des appels système
 * @param sys_num Numéro de l'appel système
 * 
 * Implémente les différents appels système:
 * - 1: Affichage de la valeur d'un compteur
 */
void __regparm__(1) syscall_handler(int sys_num) {

	uint32_t *counter;

	  if (sys_num ==1){
      	asm volatile("mov  %%ebx, %0":"=m"(counter) :);
   	  	debug("Valeur compteur: %d\n", *counter);
	  } else {
		debug("Erreur syscall inexistant");
	  }
}

/**
 * @fn void schedule(void)
 * @brief Ordonnanceur de processus
 * 
 * Réalise le changement de contexte entre processus:
 * - Sauvegarde le contexte du processus courant
 * - Sélectionne le prochain processus à exécuter
 * - Restaure le contexte du nouveau processus
 */
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

/**
 * @fn void irq0_handler()
 * @brief Gestionnaire d'interruption timer
 * 
 * Appelé à chaque interruption timer, déclenche l'ordonnanceur
 */
void irq0_handler() {
	asm volatile (
		"pusha       \n"
		"call schedule \n"
		"popa       \n"
		"add $4, %esp   \n"
		"iret       \n"
	);
}

/**
 * @fn void sys_counter(uint32_t * counter)
 * @brief Appel système pour afficher un compteur
 * @param counter Pointeur vers le compteur à afficher
 */
void sys_counter(uint32_t * counter){
      asm volatile("mov  %0, %%ebx;mov $0x01, %%eax":"=m"(counter) :);
      asm volatile ("int $0x80");
}

//-----------------------------------------------------Fonction compteurs (Ecriture et Lecture) ----------------------------

/**
 * @fn void user1()
 * @brief Incrémentation du compteur - Processus utilisateur 1
 * 
 * Processus qui incrémente un compteur en mémoire partagée
 */
__attribute__((section(".user1.text"))) void user1() {
	
	uint32_t *counter = (uint32_t *)0x706000; // Adresse virtuelle dans la zone partagée
    while (1) {
        // Incrémente le compteur
      (*counter)++;
		for(int i = 0; i<50000000; i++);
    }
}

/**
 * @fn void user2()
 * @brief Affichage du compteur - Processus utilisateur 2
 * 
 * Processus qui lit et affiche la valeur du compteur via un appel système
 */
__attribute__((section(".user2.text")))  void user2() {

    uint32_t *counter = (uint32_t *)0x806000; // Adresse virtuelle dans la zone partagée

    while (1) {
      sys_counter(counter);
		for(int i = 0; i<40000000; i++);
    }
}

//----------------------------------------------------Initialisation des tables de pages ----------------------------------------

/**
 * @fn void init_tables()
 * @brief Initialise les tables de pages des processus
 * 
 * Configure la pagination pour chaque processus:
 * - Tables de pages pour l'espace utilisateur
 * - Tables de pages pour l'espace noyau
 * - Zones de mémoire partagée
 */
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
/**
 * @fn void init_idtr()
 * @brief Initialise l'IDT (Interrupt Descriptor Table)
 * 
 * Configure:
 * - Le gestionnaire d'interruption timer (IRQ0)
 * - Le gestionnaire d'appels système (int 0x80)
 */
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

/**
 * @fn void ChargementTache(uint32_t pgd, uint32_t esp, uint32_t fonction)
 * @brief Charge un nouveau processus
 * @param pgd Adresse du répertoire de pages du processus
 * @param esp Pointeur de pile initial
 * @param fonction Point d'entrée du processus
 * 
 * Initialise une nouvelle entrée dans la table des processus avec:
 * - Configuration des registres
 * - Initialisation de la pile
 * - Configuration des segments
 */
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
/**
 * @fn void tp()
 * @brief Point d'entrée principal du système
 * 
 * Séquence d'initialisation:
 * 1. Initialisation de la GDT
 * 2. Configuration des tables de pages
 * 3. Configuration de l'IDT
 * 4. Chargement des processus utilisateur
 * 5. Activation de la pagination
 * 6. Activation des interruptions
 * 7. Passage en mode utilisateur
 */
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
