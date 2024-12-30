# Cartographie Mémoire du Système

![Cartographie mémoire](https://aguiller31.github.io/secos-ng/cartpgraphie.png)
## Vue d'ensemble

Le système utilise une pagination 32 bits avec deux espaces d'adressage distincts pour deux processus utilisateur, avec certaines zones partagées et des zones réservées au noyau.

## Schéma de la mémoire
![Mémoire](https://aguiller31.github.io/secos-ng/memory.png?text=image)
## Zones Mémoire Principales

### Tables de Pages
- **0x700000**: Page Directory du processus 1
- **0x701000**: Page Table du processus 1
- **0x702000**: Page Table kernel pour processus 1
- **0x800000**: Page Directory du processus 2
- **0x801000**: Page Table du processus 2
- **0x802000**: Page Table kernel pour processus 2

### Zones Kernel
- **0x300000-0x303000**: Zone réservée au kernel (4 pages)
- **0x400000**: Pile kernel pour processus 1
- **0x402000**: Pile kernel pour processus 2

### Processus 1
- **0x704000**: Zone de code/données
- **0x706000**: Zone de données partagée (compteur)
- **0x901000**: Pile utilisateur

### Processus 2
- **0x804000**: Zone de code/données
- **0x806000**: Zone de données partagée (mappée sur 0x706000)
- **0x903000**: Pile utilisateur

## Mécanismes de Partage

1. **Zone Partagée**: 
   - Le compteur à l'adresse 0x706000 est partagé entre les deux processus
   - Processus 1 y accède via 0x706000
   - Processus 2 y accède via 0x806000

2. **Accès Kernel**:
   - Les deux processus ont accès en lecture/écriture à leurs zones respectives
   - Les zones kernel sont protégées (PG_KRN)

## Droits d'accès

- **Zones Utilisateur**: PG_USR|PG_RW (lecture/écriture utilisateur)
- **Zones Kernel**: PG_KRN|PG_RW (lecture/écriture kernel uniquement)
- **Tables de Pages**: PG_USR|PG_RW (accessibles pour les modifications)

## Remarques Importantes

1. La mémoire partagée permet la communication inter-processus via le compteur
2. Chaque processus a sa propre pile utilisateur
3. Les zones kernel sont isolées mais accessibles via les interruptions
4. Les tables de pages sont configurées pour permettre l'isolation entre processus tout en maintenant les zones de partage nécessaires
