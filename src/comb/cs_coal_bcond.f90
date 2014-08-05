!-------------------------------------------------------------------------------

! This file is part of Code_Saturne, a general-purpose CFD tool.
!
! Copyright (C) 1998-2014 EDF S.A.
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

!===============================================================================
! Function :
! --------
!>  \file cs_coal_bcomb.f90
!>  \brief   Boundary condition automatic for pulverized coal combution
!
!-------------------------------------------------------------------------------

!-------------------------------------------------------------------------------
! Arguments
!______________________________________________________________________________.
!  mode           name          role
!______________________________________________________________________________!
!> \param[in]     itypfb        boundary face types
!> \param[in,out] izfppp        zone number for the edge face for
!>                                      the specific physic module
!> \param[in,out] rcodcl        value of the boundary conditions to edge faces
!>
!>                              boundary condition values:
!>                               - rcodcl(1) value of the dirichlet
!>                               - rcodcl(2) value of the exterior exchange
!>                               -  coefficient (infinite if no exchange)
!>                               -  rcodcl(3) value flux density
!>                               -  (negative if gain) \f$w.m^{-2} \f$ or
!>                               -  roughness in \f$m\f$ if  icodcl=6
!>                                -# for velocity:
!>                                           \f$(\mu+\mu_T)\gradv \vect{u}\f$
!>                                -# for pressure: \f$ \Delta \grad P
!>                                                 \cdot \vect{n} \f$
!>                                -# for scalar:   \f$ C_p \left ( K +
!>                                                 \dfrac{K_T}{\sigma_T} \right)
!>                                                 \grad T \cdot \vect{n} \f$
!______________________________________________________________________________!

subroutine cs_coal_bcond &
 ( itypfb , izfppp ,                                              &
   rcodcl )

!===============================================================================

!===============================================================================
! Module files
!===============================================================================

use paramx
use numvar
use optcal
use cstphy
use cstnum
use entsor
use parall
use ppppar
use ppthch
use coincl
use cpincl
use ppincl
use ppcpfu
use cs_coal_incl
use mesh
use field

!===============================================================================

implicit none

! Arguments

integer          itypfb(nfabor)
integer          izfppp(nfabor)

double precision rcodcl(nfabor,nvarcl,3)
! Local variables

character*80     name

integer          ii, ifac, izone, mode, iel, ige, iok
integer          icha, iclapc, isol, icla
integer          icke, idecal
integer          nbrval, ioxy
integer          f_id, iaggas, keyvar

double precision qisqc, viscla, d2s3, uref2, rhomoy, dhy, xiturb
double precision ustar2, xkent, xeent, t1, t2, totcp , dmas
double precision h1    (nozppm) , h2   (nozppm,nclcpm)
double precision x2h20t(nozppm) , x20t (nozppm)
double precision qimpc (nozppm) , qcalc(nozppm)
double precision coefe (ngazem)
double precision xsolid(nsolim)
double precision f1mc  (ncharm) , f2mc (ncharm)
double precision wmh2o,wmco2,wmn2,wmo2
double precision, dimension(:), pointer ::  brom
integer, dimension (:), allocatable :: iagecp
double precision, dimension(:), pointer :: viscl
!===============================================================================
! 0. Initialization
!===============================================================================
call field_get_val_s(ibrom, brom)
call field_get_val_s(iprpfl(iviscl), viscl)

d2s3 = 2.d0/3.d0

call field_get_key_id("variable_id", keyvar)

call field_get_id_try('x_age_gas', f_id)

if (f_id.ne.-1) then
  call field_get_key_int(f_id, keyvar, iaggas)

  allocate (iagecp(nclacp))

  do icla = 1, nclacp
    write(name,'(a8,i2.2)')'x_age_coal', icla
    call field_get_id(name, f_id)
    call field_get_key_int(f_id, keyvar, iagecp(icla))
  enddo
endif
!===============================================================================
! 1.  Exchanges in parallel for the user data
!===============================================================================
!  In fact this exchange could be avoided by changing uscpcl and by asking
!    the user to give the variables which depend of the area out of the loop
!    on the edge faces: the variables would be available on all processors.
!  However, it makes the user subroutine a bit more complicated and especially
!    if the user modifies it through, it does not work.
!  We assume that all the provided variables are positive,
!    which allows to use a max for the proceedings know them.
!  If this is not the case, it is more complicated but we can get a max anyway.
if(irangp.ge.0) then
  call parimx(nozapm,iqimp )
  call parimx(nozapm,ientat)
  call parimx(nozapm,ientcp)
  call parimx(nozapm,inmoxy)
  call parrmx(nozapm,qimpat)
  call parrmx(nozapm,timpat)
  nbrval = nozppm*ncharm
  call parrmx(nbrval,qimpcp)
  nbrval = nozppm*ncharm
  call parrmx(nbrval,timpcp)
  nbrval = nozppm*ncharm*ncpcmx
  call parrmx(nbrval,distch)
endif

!===============================================================================
! 2.  Correction of the velocities (in norm) for controlling the imposed flow
!       Loop over all input faces
!                     =========================
!===============================================================================
! --- Calculated flow
do izone = 1, nozppm
  qcalc(izone) = 0.d0
enddo
do ifac = 1, nfabor
  izone = izfppp(ifac)
  qcalc(izone) = qcalc(izone) - brom(ifac) *             &
                ( rcodcl(ifac,iu,1)*surfbo(1,ifac) +       &
                  rcodcl(ifac,iv,1)*surfbo(2,ifac) +       &
                  rcodcl(ifac,iw,1)*surfbo(3,ifac) )
enddo

if(irangp.ge.0) then
  call parrsm(nozapm,qcalc )
endif

do izone = 1, nozapm
  if ( iqimp(izone).eq.0 ) then
    qimpc(izone) = qcalc(izone)
  endif
enddo

! --- Correction of the velocities (in norm) from the second iteration,
!       otherwise we do not know the density at the edge
if ( ntcabs .gt. 1 ) then
  iok = 0
  do ii = 1, nzfppp
    izone = ilzppp(ii)
    if ( iqimp(izone).eq.1 ) then
      if(abs(qcalc(izone)).lt.epzero) then
        write(nfecra,2001)izone,iqimp(izone),qcalc(izone)
        iok = iok + 1
      endif
    endif
  enddo
  if(iok.ne.0) then
    call csexit (1)
  endif
  do ifac = 1, nfabor
    izone = izfppp(ifac)
    if ( iqimp(izone).eq.1 ) then
      qimpc(izone) = qimpat(izone)
      do icha = 1, ncharb
        qimpc(izone) = qimpc(izone) + qimpcp(izone,icha)
      enddo
      qisqc = qimpc(izone)/qcalc(izone)
      rcodcl(ifac,iu,1) = rcodcl(ifac,iu,1)*qisqc
      rcodcl(ifac,iv,1) = rcodcl(ifac,iv,1)*qisqc
      rcodcl(ifac,iw,1) = rcodcl(ifac,iw,1)*qisqc
    endif
  enddo

else

  do izone = 1, nozapm
    qimpc(izone) = qimpat(izone)
    do icha = 1, ncharb
      qimpc(izone) = qimpc(izone) + qimpcp(izone,icha)
    enddo
  enddo

endif


 2001 format(                                                     &
'@                                                            ',/,&
'@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@',/,&
'@                                                            ',/,&
'@ @@ WARNING : SPECIFIC PHYSIC MODULE                        ',/,&
'@    =========                        pulverized coal        ',/,&
'@    problem in the boundary conditions                      ',/,&
'@                                                            ',/,&
'@  The flow rate is imposed on the area izone =  ', I10       ,/,&
'@    because                iqimp(izone) =     ', I10         ,/,&
'@  However, on this area, the integrated product rho D S     ',/,&
'@    is zero                             :                   ',/,&
'@    it is                               = ',E14.5            ,/,&
'@    (D is the direction in which the flow is imposed).      ',/,&
'@                                                            ',/,&
'@  The calculation can not be executed                       ',/,&
'@                                                            ',/,&
'@  Check uscpcl, and in particular                           ',/,&
'@    - that the vector  rcodcl(ifac,iu,1)                    ',/,&
'@                       rcodcl(ifac,iv,1),                   ',/,&
'@                       rcodcl ifac,iw,1) which determines   ',/,&
'@      the direction of the velocity is not zero and is not  ',/,&
'@      uniformly perpendicular to the imput faces            ',/,&
'@    - that the surface of the imput is not zero (or the     ',/,&
'@      number of edge faces in the area is non-zero)         ',/,&
'@    - that the density is not zero                          ',/,&
'@                                                            ',/,&
'@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@',/,&
'@                                                            ',/)

!===============================================================================
! 3. Verifications
!        Sum coal distribution = 100% for area ientcp = 1
!===============================================================================
iok = 0
do ii = 1, nzfppp
  izone = ilzppp(ii)
  if ( ientcp(izone).eq.1 ) then
    do icha = 1, ncharb
      totcp = 0.d0
      do iclapc = 1, nclpch(icha)
        totcp = totcp + distch(izone,icha,iclapc)
      enddo
      if(abs(totcp-100.d0).gt.epzero) then
        write(nfecra,2010)
        do iclapc = 1, nclpch(icha)
          write(nfecra,2011)izone,icha,iclapc,                    &
               distch(izone,icha,iclapc)
        enddo
        write(nfecra,2012)izone,ientcp(izone),icha,               &
             totcp,totcp-100.d0
        iok = iok + 1
      endif
    enddo
  endif
enddo

if(iok.ne.0) then
  call csexit (1)
endif


 2010 format(                                                           &
'@                                                            ',/,&
'@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@',/,&
'@                                                            ',/,&
'@ @@ WARNING : SPECIFIC PHYSIC MODULE                        ',/,&
'@    =========                        pulverized coal        ',/,&
'@    probleme in the boundary conditions                     ',/,&
'@                                                            ',/,&
'@        Zone    Coal     Class         Distch(%)        '  )
 2011 format(                                                           &
'@  ',I10   ,' ',I10   ,' ',I10   ,'    ',E14.5                  )
 2012 format(                                                           &
'@                                                            ',/,&
'@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@',/,&
'@                                                            ',/,&
'@ @@ WARNING : SPECIFIC PHYSIC MODULE                        ',/,&
'@    =========                        pulverized coal        ',/,&
'@    probleme in the boundary conditions                     ',/,&
'@                                                            ',/,&
'@  A coal input is imposed in izone = ', I10                  ,/,&
'@    because               ientcp(izone) = ', I10             ,/,&
'@  However, on this area, the sum of distributions by class  ',/,&
'@    in percentage for coal         icha = ', I10             ,/,&
'@    is different from 100% : it is     totcp = ', E14.5      ,/,&
'@    with                           totcp-100 = ', E14.5      ,/,&
'@                                                            ',/,&
'@  The calcul will not run                                   ',/,&
'@                                                            ',/,&
'@  Check    uscpcl.                                          ',/,&
'@                                                            ',/,&
'@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@',/,&
'@                                                            ',/)

!===============================================================================
! 4.  Filling the table of the boundary conditions
!       Loop on all input faces
!                     =========================
!         Determining the family and its properties
!         Imposing boundary conditions for the turbulence

!===============================================================================
do ifac = 1, nfabor

  izone = izfppp(ifac)

  ! Neighboring element to the edge face
  if ( itypfb(ifac).eq.ientre ) then
    ! ----  Automatic processing of turbulence

    !       The turbulence is calculated by default if icalke different from 0
    !          - or from hydraulic diameter and a reference velocity adapted
    !            for the current input if icalke = 1
    !          - either from the hydraulic diameter, a reference velocity and
    !            a turbulence intensity adapted to the current input if icalke = 2
    if ( icalke(izone).ne.0 ) then

      uref2 = rcodcl(ifac,iu,1)**2                         &
            + rcodcl(ifac,iv,1)**2                         &
            + rcodcl(ifac,iw,1)**2
      uref2 = max(uref2,1.d-12)
      rhomoy = brom(ifac)
      iel    = ifabor(ifac)
      viscla = viscl(iel)
      icke   = icalke(izone)
      dhy    = dh(izone)
      xiturb = xintur(izone)
      ustar2 = 0.d0
      xkent = epzero
      xeent = epzero
      if (icke.eq.1) then
        call keendb                                               &
        ( uref2, dhy, rhomoy, viscla, cmu, xkappa,                &
          ustar2, xkent, xeent )
      else if (icke.eq.2) then
        call keenin                                               &
        ( uref2, xiturb, dhy, cmu, xkappa, xkent, xeent )
      endif

      if (itytur.eq.2) then

        rcodcl(ifac,ik,1)  = xkent
        rcodcl(ifac,iep,1) = xeent

      elseif (itytur.eq.3) then

        rcodcl(ifac,ir11,1) = d2s3*xkent
        rcodcl(ifac,ir22,1) = d2s3*xkent
        rcodcl(ifac,ir33,1) = d2s3*xkent
        rcodcl(ifac,ir12,1) = 0.d0
        rcodcl(ifac,ir13,1) = 0.d0
        rcodcl(ifac,ir23,1) = 0.d0
        rcodcl(ifac,iep,1)  = xeent

      elseif (iturb.eq.50) then

        rcodcl(ifac,ik,1)   = xkent
        rcodcl(ifac,iep,1)  = xeent
        rcodcl(ifac,iphi,1) = d2s3
        rcodcl(ifac,ifb,1)  = 0.d0

      elseif (iturb.eq.60) then

        rcodcl(ifac,ik,1)   = xkent
        rcodcl(ifac,iomg,1) = xeent/cmu/xkent

      endif

    endif

  endif

enddo

!===============================================================================
! 5.  Filling the table  of the boundary conditions
!       Loop on all input faces
!                     =========================
!         Determining the family and its properties
!         Imposing boundary conditions for scalars
!===============================================================================
do ii = 1, nzfppp

  izone = ilzppp(ii)
  ! One input ientre is necessarily the type
  !            ientat = 1 or ientcp = 1
  if ( ientat(izone).eq.1 .or. ientcp(izone).eq.1) then

    x20t  (izone) = zero
    x2h20t(izone) = zero

    idecal = 0

    do icha = 1, ncharb

      do iclapc = 1, nclpch(icha)

        icla = iclapc + idecal
        ! ------ Calculating X2 total per area
        !         Small correction in case the input is close
        if(abs(qimpc(izone)).lt.epzero) then
          x20(izone,icla) = 0.d0
        else
          x20(izone,icla) = qimpcp(izone,icha)/qimpc(izone)       &
                          * distch(izone,icha,iclapc)*1.d-2
        endif
        x20t(izone)     = x20t(izone) +  x20(izone,icla)
        ! ------ Calculating H2 of class icla
        do isol = 1, nsolim
          xsolid(isol) = zero
        enddo
        if ( ientcp(izone).eq.1 ) then
          t2  = timpcp(izone,icha)
          xsolid(ich(icha)) = 1.d0-xashch(icha)
          xsolid(ick(icha)) = zero
          xsolid(iash(icha)) = xashch(icha)
          !------- Taking into account humidity
          if ( ippmod(iccoal) .eq. 1 ) then
            xsolid(ich(icha)) = xsolid(ich(icha))-xwatch(icha)
            xsolid(iwat(icha)) = xwatch(icha)
          else
            xsolid(iwat(icha)) = 0.d0
          endif

        else
          t2  = timpat(izone)

          xsolid(ich(icha))  = (1.d0-xashch(icha)-xwatch(icha))
          xsolid(ick(icha))  = 0.d0
          xsolid(iash(icha)) = xashch(icha)
          xsolid(iwat(icha)) = xwatch(icha)

        endif
        mode = -1
        t1 = t2
        call cs_coal_htconvers2(mode,icla,h2(izone,icla),xsolid,t2,t1)
        x2h20t(izone) = x2h20t(izone)+x20(izone,icla)*h2(izone,icla)

      enddo

      idecal = idecal + nclpch(icha)

    enddo

    ! ------ Calculating H1(izone)
    do ige = 1, ngazem
      coefe(ige) = zero
    enddo

    ioxy = inmoxy(izone)
    dmas = wmole(io2) *oxyo2(ioxy) +wmole(in2) *oxyn2(ioxy)       &
          +wmole(ih2o)*oxyh2o(ioxy)+wmole(ico2)*oxyco2(ioxy)

    coefe(io2)  = wmole(io2 )*oxyo2(ioxy )/dmas
    coefe(ih2o) = wmole(ih2o)*oxyh2o(ioxy)/dmas
    coefe(ico2) = wmole(ico2)*oxyco2(ioxy)/dmas
    coefe(in2)  = wmole(in2 )*oxyn2(ioxy )/dmas

    do icha = 1, ncharm
      f1mc(icha) = zero
      f2mc(icha) = zero
    enddo
    t1   = timpat(izone)
    mode = -1
    call cs_coal_htconvers1(mode,h1(izone),coefe,f1mc,f2mc,t1)

  endif

enddo

do ifac = 1, nfabor

  izone = izfppp(ifac)

  !      Adjacent element of the edge face
  if ( itypfb(ifac).eq.ientre ) then

    ! ----  Automatic processing of specific physic scalar

    idecal = 0

    do icha = 1, ncharb

      do iclapc = 1, nclpch(icha)

        icla = iclapc + idecal

        ! ------ Boundary conditions for Xch of class icla
        rcodcl(ifac,isca(ixch(icla)),1) = x20(izone,icla)         &
                                        * (1.d0-xashch(icha))
        !             Taking into account humidity
        if ( ippmod(iccoal) .eq. 1 ) then
          rcodcl(ifac,isca(ixch(icla)),1) = x20(izone,icla)       &
                                          *(1.d0-xashch(icha)     &
                                                -xwatch(icha))
        endif
        ! ------ Boundary conditions for Xck of class icla
        rcodcl(ifac,isca(ixck(icla)),1) = 0.d0

        ! ------ Boundary conditions for Np of class icla

        rcodcl(ifac,isca(inp(icla)),1) = x20(izone,icla)          &
                                        / xmp0(icla)

        ! ------ Boundary conditions for Xwater of class icla

        if ( ippmod(iccoal) .eq. 1 ) then
          rcodcl(ifac,isca(ixwt(icla)),1) = x20(izone,icla)       &
                                           *xwatch(icha)
        endif

        ! ------ Boundary conditions for H2 of class icla

        rcodcl(ifac,isca(ih2(icla)),1) = x20(izone,icla)          &
                                        *h2(izone,icla)
        if (i_coal_drift.eq.1) then
          rcodcl(ifac, iagecp(icla), 1) = zero
        endif
      enddo

      idecal = idecal + nclpch(icha)

      ! ------ Boundary conditions for X1F1M and X1F2M from coal icha

      rcodcl(ifac,isca(if1m(icha)),1) = zero
      rcodcl(ifac,isca(if2m(icha)),1) = zero

    enddo
    if (i_coal_drift.eq.1) then
      rcodcl(ifac, iaggas, 1) = zero
    endif

    ! ------ Boundary conditions for HM
    rcodcl(ifac,isca(iscalt),1) = (1.d0-x20t(izone))*h1(izone)    &
                                 +x2h20t(izone)
    ! ------ Boundary conditions for X1.F4M (Oxyd 2)
    if ( noxyd .ge. 2 ) then
      if ( inmoxy(izone) .eq. 2 ) then
        rcodcl(ifac,isca(if4m),1)   = (1.d0-x20t(izone))
      else
        rcodcl(ifac,isca(if4m),1)   = zero
      endif
    endif

    ! ------ Boundary conditions for X1.F5M (Oxyd3)

    if ( noxyd .eq. 3 ) then
      if ( inmoxy(izone) .eq. 3 ) then
        rcodcl(ifac,isca(if5m),1)   = (1.d0-x20t(izone))
      else
        rcodcl(ifac,isca(if5m),1)   = zero
      endif
    endif

    ! ------ Boundary conditions for X1.F6M (Water)

    if ( ippmod(iccoal) .ge. 1 ) then
      rcodcl(ifac,isca(if6m),1) = zero
    endif

    ! ------ Boundary conditions for X1.F7M_O2

    rcodcl(ifac,isca(if7m),1)   = zero

    ! ------ Boundary conditions for X1.FM8_CO2

    if ( ihtco2 .eq. 1 ) then
      rcodcl(ifac,isca(if8m),1) = zero
    endif
    ! ------ Boundary conditions for X1.FM9_H2O
    if ( ihth2o .eq. 1 ) then
      rcodcl(ifac,isca(if9m),1) = zero
    endif
    ! ------ Boundary conditions for X1.Variance
    rcodcl(ifac,isca(ifvp2m),1) = zero

    ! ------ Boundary conditions for X1.YCO2
    if ( ieqco2 .eq. 1 ) then
      ioxy =  inmoxy(izone)
      wmo2   = wmole(io2)
      wmco2  = wmole(ico2)
      wmh2o  = wmole(ih2o)
      wmn2   = wmole(in2)
      dmas = ( oxyo2 (ioxy)*wmo2 +oxyn2 (ioxy)*wmn2               &
              +oxyh2o(ioxy)*wmh2o+oxyco2(ioxy)*wmco2 )
      xco2 = oxyco2(ioxy)*wmco2/dmas
      rcodcl(ifac,isca(iyco2),1)   = xco2*(1.d0-x20t(izone))
    endif
    ! ------ Boundary conditions for X1.HCN, X1.NO, Taire
    if( ieqnox .eq. 1 ) then
      rcodcl(ifac,isca(iyhcn ),1)  = zero
      rcodcl(ifac,isca(iyno  ),1)  = zero
      rcodcl(ifac,isca(iynh3 ),1)  = zero
      rcodcl(ifac,isca(ihox  ),1)  = (1.d0-x20t(izone))*h1(izone)
    endif

  endif

enddo

! Free memory
if (allocated(iagecp)) deallocate(iagecp)
!--------
! Formats
!--------

!----
! End
!----

return
end subroutine
