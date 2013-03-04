!-------------------------------------------------------------------------------

! This file is part of Code_Saturne, a general-purpose CFD tool.
!
! Copyright (C) 1998-2013 EDF S.A.
!
! This program is free software; you can redistribute it and/or modify it under
! the terms of the GNU General Public License as published by the Free Software
! Foundation; either version 2 of the License, or (at your option) any later
! version.
!
! This program is distributed in the hope that it will be useful, but WITHOUT
! ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
! FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
! details.
!
! You should have received a copy of the GNU General Public License along with
! this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
! Street, Fifth Floor, Boston, MA 02110-1301, USA.

!-------------------------------------------------------------------------------

subroutine altycl &
!================

 ( nvar   , nscal  ,                                              &
   itypfb , ialtyb , icodcl , impale ,                            &
   dt     , rtp    , rtpa   , propce , propfa , propfb ,          &
   rcodcl , xyzno0 , depale )

!===============================================================================
! FONCTION :
! --------

! TRAITEMENT DES CODES DE CONDITIONS AUX LIMITES IALTYB POUR L'ALE

!-------------------------------------------------------------------------------
! Arguments
!__________________.____._____.________________________________________________.
! name             !type!mode ! role                                           !
!__________________!____!_____!________________________________________________!
! nvar             ! i  ! <-- ! total number of variables                      !
! nscal            ! i  ! <-- ! total number of scalars                        !
! itypfb           ! ia ! <-- ! boundary face types                            !
! ialtyb(nfabor    ! te ! <-- ! type des faces de bord pour l'ale              !
! icodcl           ! te ! <-- ! code de condition limites aux faces            !
!  (nfabor,nvar    !    !     !  de bord                                       !
!                  !    !     ! = 1   -> dirichlet                             !
!                  !    !     ! = 3   -> densite de flux                       !
! impale(nnod)     ! te ! <-- ! indicateur de delacement impose                !
! dt(ncelet)       ! ra ! <-- ! time step (per cell)                           !
! rtp, rtpa        ! ra ! <-- ! calculated variables at cell centers           !
!  (ncelet, *)     !    !     !  (at current and previous time steps)          !
! propce(ncelet, *)! ra ! <-- ! physical properties at cell centers            !
! propfa(nfac, *)  ! ra ! <-- ! physical properties at interior face centers   !
! propfb(nfabor, *)! ra ! <-- ! physical properties at boundary face centers   !
! rcodcl           ! tr ! <-- ! valeur des conditions aux limites              !
!  (nfabor,nvar    !    !     !  aux faces de bord                             !
!                  !    !     ! rcodcl(1) = valeur du dirichlet                !
!                  !    !     ! rcodcl(2) = valeur du coef. d'echange          !
!                  !    !     !  ext. (infinie si pas d'echange)               !
!                  !    !     ! rcodcl(3) = valeur de la densite de            !
!                  !    !     !  flux (negatif si gain) w/m2                   !
!                  !    !     ! pour les vitesses (vistl+visct)*gradu          !
!                  !    !     ! pour la pression             dt*gradp          !
!                  !    !     ! pour les scalaires                             !
!                  !    !     !        cp*(viscls+visct/sigmas)*gradt          !
! depale(nnod,3    ! tr ! <-- ! deplacement aux noeuds                         !
! xyzno0(3,nnod    ! tr ! <-- ! coordonnees noeuds maillage initial            !
!__________________!____!_____!________________________________________________!

!     TYPE : E (ENTIER), R (REEL), A (ALPHANUMERIQUE), T (TABLEAU)
!            L (LOGIQUE)   .. ET TYPES COMPOSES (EX : TR TABLEAU REEL)
!     MODE : <-- donnee, --> resultat, <-> Donnee modifiee
!            --- tableau de travail
!===============================================================================

!===============================================================================
! Module files
!===============================================================================

use paramx
use dimens, only: ndimfb
use numvar
use optcal
use cstnum
use cstphy
use entsor
use parall
use mesh

!===============================================================================

implicit none

! Arguments

integer          nvar   , nscal

integer          itypfb(nfabor)
integer          ialtyb(nfabor), icodcl(nfabor,nvarcl)
integer          impale(nnod)

double precision dt(ncelet), rtp(ncelet,*), rtpa(ncelet,*)
double precision propce(ncelet,*)
double precision propfa(nfac,*), propfb(ndimfb,*)
double precision rcodcl(nfabor,nvarcl,3)
double precision depale(nnod,3), xyzno0(3,nnod)

! Local variables

integer          ifac, iel
integer          ii, inod, iecrw, icpt, ierror(4), icoder(4), irkerr
double precision ddepx, ddepy, ddepz
double precision srfbnf, rnx, rny, rnz
double precision rcodcx, rcodcy, rcodcz, rcodsn


!===============================================================================

!===============================================================================
! 1.  INITIALISATIONS
!===============================================================================

irkerr = -1

ierror(1) = 0
ierror(2) = 0
ierror(3) = 0
ierror(4) = 0

icoder(1) = -1
icoder(2) = -1
icoder(3) = -1
icoder(4) = -1

! Mise a zero des RCODCL non specifies
do ifac = 1, nfabor
  if (rcodcl(ifac,iuma,1).gt.rinfin*0.5d0)                        &
       rcodcl(ifac,iuma,1) = 0.d0
  if (rcodcl(ifac,ivma,1).gt.rinfin*0.5d0)                        &
       rcodcl(ifac,ivma,1) = 0.d0
  if (rcodcl(ifac,iwma,1).gt.rinfin*0.5d0)                        &
       rcodcl(ifac,iwma,1) = 0.d0
enddo

!===============================================================================
! 2.  VERIFICATION DE LA CONSISTANCE DES IALTYB DONNES DANS USALCL
!===============================================================================
!  (valeur 0 autorisee)

do ifac = 1, nfabor
  if (ialtyb(ifac).ne.0      .and.                                &
      ialtyb(ifac).ne.ibfixe .and.                                &
      ialtyb(ifac).ne.igliss .and.                                &
      ialtyb(ifac).ne.ifresf .and.                                &
      ialtyb(ifac).ne.ivimpo ) then
    if (ialtyb(ifac).gt.0) then
      ialtyb(ifac) = -ialtyb(ifac)
    endif
    ierror(1) = ierror(1) + 1
  endif
enddo

if (irangp.ge.0) call parcmx(ierror(1))
if(ierror(1).ne.0) then
  write(nfecra,1000)
  call bcderr(ialtyb)
endif

!===============================================================================
! 3.  CONVERSION EN RCODCL ICODCL
!===============================================================================

! Calcul ou ecrasement de RCODCL a partir du deplacement impose,
!   si tous les noeuds d'une face sont a deplacement impose

do ifac = 1, nfabor
  iecrw = 0
  ddepx = 0.d0
  ddepy = 0.d0
  ddepz = 0.d0
  icpt  = 0
  do ii = ipnfbr(ifac), ipnfbr(ifac+1)-1
    inod = nodfbr(ii)
    if (impale(inod).eq.0) iecrw = iecrw + 1
    icpt = icpt + 1
    ddepx = ddepx + depale(inod,1)+xyzno0(1,inod)-xyznod(1,inod)
    ddepy = ddepy + depale(inod,2)+xyzno0(2,inod)-xyznod(2,inod)
    ddepz = ddepz + depale(inod,3)+xyzno0(3,inod)-xyznod(3,inod)
  enddo
  if (iecrw.eq.0) then
    iel = ifabor(ifac)
    ialtyb(ifac) = ivimpo
    rcodcl(ifac,iuma,1) = ddepx/dt(iel)/icpt
    rcodcl(ifac,ivma,1) = ddepy/dt(iel)/icpt
    rcodcl(ifac,iwma,1) = ddepz/dt(iel)/icpt
  endif
enddo

! Remplissage des autres RCODCL a partir des ITYALB

do ifac = 1, nfabor

  iel = ifabor(ifac)

! --> Faces fixes
!     On force alors les noeuds en question a etre fixes, pour eviter
!       des problemes eventuels aux coins

  if (ialtyb(ifac).eq.ibfixe) then
    icpt = 0
    if (icodcl(ifac,iuma).eq.0) then
      icpt = icpt + 1
      icodcl(ifac,iuma) = 1
      rcodcl(ifac,iuma,1) = 0.d0
    endif
    if (icodcl(ifac,ivma).eq.0) then
      icpt = icpt + 1
      icodcl(ifac,ivma) = 1
      rcodcl(ifac,ivma,1) = 0.d0
    endif
    if (icodcl(ifac,iwma).eq.0) then
      icpt = icpt + 1
      icodcl(ifac,iwma) = 1
      rcodcl(ifac,iwma,1) = 0.d0
    endif
!      Si on a fixe les trois composantes, alors on fixe les noeuds
!        correspondants. Sinon c'est que l'utilisateur a modifie quelque
!        chose     ... on le laisse seul maitre
    if (icpt.eq.3) then
      do ii = ipnfbr(ifac), ipnfbr(ifac+1)-1
        inod = nodfbr(ii)
        if (impale(inod).eq.0) then
          depale(inod,1) = 0.d0
          depale(inod,2) = 0.d0
          depale(inod,3) = 0.d0
          impale(inod) = 1
        endif
      enddo
    endif


! --> Faces de glissement
  elseif (ialtyb(ifac).eq.igliss) then

    if (icodcl(ifac,iuma).eq.0) icodcl(ifac,iuma) = 4
    if (icodcl(ifac,ivma).eq.0) icodcl(ifac,ivma) = 4
    if (icodcl(ifac,iwma).eq.0) icodcl(ifac,iwma) = 4

! --> Faces a vitesse imposee
  elseif (ialtyb(ifac).eq.ivimpo) then

    if (icodcl(ifac,iuma).eq.0) icodcl(ifac,iuma) = 1
    if (icodcl(ifac,ivma).eq.0) icodcl(ifac,ivma) = 1
    if (icodcl(ifac,iwma).eq.0) icodcl(ifac,iwma) = 1

! --> Free surface face: the mesh velocity is imposed by the mass flux
!TODO set default condition on the fluid velocity to homogenous Neumann
  elseif (ialtyb(ifac).eq.ifresf) then

    if (icodcl(ifac,iuma).eq.0) then
      icodcl(ifac,iuma) = 1
    endif
    if (icodcl(ifac,ivma).eq.0) then
      icodcl(ifac,ivma) = 1
    endif
    if (icodcl(ifac,iwma).eq.0) then
      icodcl(ifac,iwma) = 1
    endif

  endif

enddo

!===============================================================================
! 4.  VERIFICATION DE LA CONSISTANCE DES ICODCL
!===============================================================================

!     IERROR a ete initialise plus haut
do ifac = 1, nfabor

  if (icodcl(ifac,iuma).ne.1 .and.                                &
      icodcl(ifac,iuma).ne.3 .and.                                &
      icodcl(ifac,iuma).ne.4 ) then
    if (ialtyb(ifac).gt.0) then
      ialtyb(ifac) = -ialtyb(ifac)
    endif
    ierror(1) = ierror(1) + 1
  endif
  if (icodcl(ifac,ivma).ne.1 .and.                                &
      icodcl(ifac,ivma).ne.3 .and.                                &
      icodcl(ifac,ivma).ne.4 ) then
    if (ialtyb(ifac).gt.0) then
      ialtyb(ifac) = -ialtyb(ifac)
    endif
    ierror(2) = ierror(2) + 1
  endif
  if (icodcl(ifac,iwma).ne.1 .and.                                &
      icodcl(ifac,iwma).ne.3 .and.                                &
      icodcl(ifac,iwma).ne.4 ) then
    if (ialtyb(ifac).gt.0) then
      ialtyb(ifac) = -ialtyb(ifac)
    endif
    ierror(3) = ierror(3) + 1
  endif

  if ( ( icodcl(ifac,iuma).eq.4 .or.                              &
         icodcl(ifac,ivma).eq.4 .or.                              &
         icodcl(ifac,iwma).eq.4    ) .and.                        &
       ( icodcl(ifac,iuma).ne.4 .or.                              &
         icodcl(ifac,ivma).ne.4 .or.                              &
         icodcl(ifac,iwma).ne.4    ) ) then
    if (ialtyb(ifac).gt.0) then
      ialtyb(ifac) = -ialtyb(ifac)
    endif
    ierror(4) = ierror(4) + 1
  endif

  if (ialtyb(ifac).lt.0) then
    irkerr = irangp
    icoder(1) = -ialtyb(ifac)
    icoder(2) = icodcl(ifac,iuma)
    icoder(3) = icodcl(ifac,iuma)
    icoder(4) = icodcl(ifac,iuma)
  endif

enddo

if (irangp.ge.0) call parimx(4, ierror)

if (ierror(1).gt.0) then
  write(nfecra,2000) 'x', 'iuma'
endif
if (ierror(2).gt.0) then
  ierror(1) = 1
  write(nfecra,2000) 'y', 'ivma'
endif
if (ierror(3).gt.0) then
  ierror(1) = 1
  write(nfecra,2000) 'z', 'iwma'
endif
if (ierror(4).gt.0) then
  ierror(1) = 1
  if (irangp.ge.0) then
    call parimx(1, irkerr)
    call parbci(irkerr, 4, icoder)
    write(nfecra,3000) icoder(1), icoder(2), icoder(3), icoder(4)
  endif
endif

if (ierror(1).gt.0) then
  write(nfecra,4000)
  call bcderr(ialtyb)
endif

!===============================================================================
! 5.  VITESSE DE DEFILEMENT POUR LES PAROIS FLUIDES ET SYMETRIES
!===============================================================================

! Pour les symetries on rajoute toujours la vitesse de maillage, car on
!   ne conserve que la vitesse normale
! Pour les parois, on prend la vitesse de maillage si l'utilisateur n'a
!   pas specifie RCODCL, sinon on laisse RCODCL pour la vitesse tangente
!   et on prend la vitesse de maillage pour la composante normale.
! On se base uniquement sur ITYPFB, a l'utilisateur de gere les choses
!   s'il rentre en CL non standards.

do ifac = 1, nfabor

  if (ialtyb(ifac).eq.ivimpo) then

    if ( itypfb(ifac).eq.isymet ) then
      rcodcl(ifac,iu,1) = rcodcl(ifac,iuma,1)
      rcodcl(ifac,iv,1) = rcodcl(ifac,ivma,1)
      rcodcl(ifac,iw,1) = rcodcl(ifac,iwma,1)
    endif

    if ( itypfb(ifac).eq.iparoi .or.                        &
         itypfb(ifac).eq.iparug ) then
! Si une des composantes de vitesse de glissement a ete
!    modifiee par l'utilisateur, on ne fixe que la vitesse
!    normale
      if (rcodcl(ifac,iu,1).gt.rinfin*0.5d0 .and.              &
          rcodcl(ifac,iv,1).gt.rinfin*0.5d0 .and.              &
          rcodcl(ifac,iw,1).gt.rinfin*0.5d0) then
        rcodcl(ifac,iu,1) = rcodcl(ifac,iuma,1)
        rcodcl(ifac,iv,1) = rcodcl(ifac,ivma,1)
        rcodcl(ifac,iw,1) = rcodcl(ifac,iwma,1)
      else
! On met a 0 les composantes de RCODCL non specifiees
        if (rcodcl(ifac,iu,1).gt.rinfin*0.5d0)                 &
             rcodcl(ifac,iu,1) = 0.d0
        if (rcodcl(ifac,iv,1).gt.rinfin*0.5d0)                 &
             rcodcl(ifac,iv,1) = 0.d0
        if (rcodcl(ifac,iw,1).gt.rinfin*0.5d0)                 &
             rcodcl(ifac,iw,1) = 0.d0

        srfbnf = surfbn(ifac)
        rnx = surfbo(1,ifac)/srfbnf
        rny = surfbo(2,ifac)/srfbnf
        rnz = surfbo(3,ifac)/srfbnf
        rcodcx = rcodcl(ifac,iu,1)
        rcodcy = rcodcl(ifac,iv,1)
        rcodcz = rcodcl(ifac,iw,1)
        rcodsn = (rcodcl(ifac,iuma,1)-rcodcx)*rnx                 &
             +   (rcodcl(ifac,ivma,1)-rcodcy)*rny                 &
             +   (rcodcl(ifac,ivma,1)-rcodcz)*rnz
        rcodcl(ifac,iu,1) = rcodcx + rcodsn*rnx
        rcodcl(ifac,iv,1) = rcodcy + rcodsn*rny
        rcodcl(ifac,iw,1) = rcodcz + rcodsn*rnz
      endif
    endif

  endif

enddo

!===============================================================================
! FORMATS
!===============================================================================

#if defined(_CS_LANG_FR)

 1000 format(                                                     &
'@'                                                            ,/,&
'@ METHODE ALE'                                                ,/,&
'@'                                                            ,/,&
'@ Au moins une face de bord a un type de bord non reconnu.'  ,/,&
'@    Le calcul ne sera pas execute.'                          ,/,&
'@'                                                            ,/,&
'@ Verifier les conditions aux limites dans l''Interface'      ,/,&
'@    ou dans usalcl.f90.'                                     ,/,&
'@'                                                              )
 2000 format(                                                     &
'@'                                                            ,/,&
'@ METHODE ALE'                                                ,/,&
'@'                                                            ,/,&
'@ Au moins une face de bord a un type de condition a la'      ,/,&
'@    limite (icodcl) non reconnu pour la vitesse de maillage' ,/,&
'@    selon' ,a1,' (',a4,').'                                  ,/,&
'@'                                                            ,/,&
'@ Les seules valeurs autorisees pour icodcl sont'             ,/,&
'@   1 : Dirichlet'                                            ,/,&
'@   3 : Neumann'                                              ,/,&
'@   4 : Glissement'                                           ,/,&
'@'                                                              )
 3000 format(                                                     &
'@'                                                            ,/,&
'@ METHODE ALE'                                                ,/,&
'@'                                                            ,/,&
'@ Incoherence dans les types de conditions a la limite'       ,/,&
'@   pour la vitesse de maillage.'                              ,/,&
'@'                                                            ,/,&
'@ Au moins une face de bord a les conditions aux limites'     ,/,&
'@   suivantes :'                                              ,/,&
'@'                                                            ,/,&
'@   itypcl : ',i10                                            ,/,&
'@   icodcl(.,iuma) : ', i10                                   ,/,&
'@   icodcl(.,ivma) : ', i10                                   ,/,&
'@   icodcl(.,iwma) : ', i10                                   ,/,&
'@'                                                            ,/,&
'@ Si une composante est traitee en glissement (icodcl=4),'    ,/,&
'@   toutes les composantes doivent etre traitees en'          ,/,&
'@   glissement.'                                              ,/,&
'@'                                                              )
 4000 format(                                                     &
'@'                                                            ,/,&
'@ METHODE ALE'                                                ,/,&
'@'                                                            ,/,&
'@ Incoherence dans les conditions aux limites pour la vitesse',/,&
'@   de maillage'                                              ,/,&
'@   (cf. message(s) ci-dessus)'                               ,/,&
'@'                                                            ,/,&
'@ Verifier dans l''interface ou dans usalcl.f90'              ,/,&
'@'                                                              )

#else

 1000 format(                                                     &
'@'                                                            ,/,&
'@ ALE METHOD'                                                 ,/,&
'@'                                                            ,/,&
'@ At least one boundary face has an unknown boundary type.'   ,/,&
'@'                                                            ,/,&
'@    The calculation will not be run.'                        ,/,&
'@'                                                            ,/,&
'@ Check boundary conditions in the GUI or in usalcl.f90.'     ,/,&
'@'                                                              )
 2000 format(                                                     &
'@'                                                            ,/,&
'@ ALE METHOD'                                                 ,/,&
'@'                                                            ,/,&
'@ At least one boundary face has a boundary condition type'   ,/,&
'@    (icodcl) which is not recognized for mesh velocity'      ,/,&
'@    along' ,a1,' (',a4,').'                                  ,/,&
'@'                                                            ,/,&
'@ The only allowed values for icodcl are'                     ,/,&
'@   1 : Dirichlet'                                            ,/,&
'@   3 : Neumann'                                              ,/,&
'@   4 : Slip'                                                 ,/,&
'@'                                                              )
 3000 format(                                                     &
'@'                                                            ,/,&
'@ ALE METHOD'                                                 ,/,&
'@'                                                            ,/,&
'@ Inconsistency in boundary condition types'                  ,/,&
'@   for mesh velocity.'                                       ,/,&
'@'                                                            ,/,&
'@ At least one boundary face has the following boundary'      ,/,&
'@   conditions:'                                              ,/,&
'@'                                                            ,/,&
'@   itypcl : ',i10                                            ,/,&
'@   icodcl(.,iuma) : ', i10                                   ,/,&
'@   icodcl(.,ivma) : ', i10                                   ,/,&
'@   icodcl(.,iwma) : ', i10                                   ,/,&
'@'                                                            ,/,&
'@ If one component has slip conditions(icodcl=4),'            ,/,&
'@   all components must have slip conditions.'                ,/,&
'@'                                                              )
 4000 format(                                                     &
'@'                                                            ,/,&
'@ ALE METHOD'                                                 ,/,&
'@'                                                            ,/,&
'@ Inconsistency in boundary condition types'                  ,/,&
'@   for mesh velocity.'                                       ,/,&
'@   (cf. message(s) above)'                                   ,/,&
'@'                                                            ,/,&
'@ Check boundary conditions in the GUI or in usalcl.f90.'     ,/,&
'@'                                                              )

#endif

return
end subroutine
