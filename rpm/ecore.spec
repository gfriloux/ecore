Name         : ecore
Version      : 1.7.11
Release      : 1%{?dist}
License      : BSD
Group        : System Environment/Libraries
Provides     : ecore = %{version}, libecore1 = %{version}
URL          : http://enlightenment.org/
Packager     : Guillaume Friloux <guillaume@friloux.me>
Group        : System/Libraries
Summary      : Enlightened Core event loop library
Source       : ~/rpmbuild/SOURCES/%{name}-%{version}.tar.gz

BuildRequires: pkgconfig subversion automake doxygen m4 autoconf gzip bzip2 tar
BuildRequires: eina-devel eet-devel

%description
Core EFL (Enlightenment Foundation Library) to handle various data types.

%package devel
Summary: Ecore headers and development libraries.
Group: Development/Libraries
Requires: %{name} = %{version}
Requires: curl-devel, openssl-devel, eet-devel
Requires: ecore-con, ecore-file, ecore-ipc
#Requires: ecore-evas, evas-devel
#Requires: ecore-x %{?with_lib_ecore_fb:ecore-fb} %{?with_lib_ecore_directfb:ecore-directfb}

%description devel
Ecore development files

%package con
Summary: Ecore Connection Library
Group: Development/Libraries
Requires: %{name} = %{version}

%description con
Ecore Connection Library

%if %{with lib_ecore_directfb}
%package directfb
Summary: Ecore DirectFB system functions
Group: Development/Libraries
Requires: %{name} = %{version}
%description directfb
Ecore DirectFB system functions
%endif

%package evas
Summary: Ecore Evas Wrapper Library
Group: Development/Libraries
Requires: %{name} = %{version}

%description evas
Ecore Evas Wrapper Library

%if %{with lib_ecore_fb}
%package fb
Summary: Ecore frame buffer system functions
Group: Development/Libraries
Requires: %{name} = %{version}
%description fb
Ecore frame buffer system functions
%endif

%package file
Summary: Ecore File Library
Group: Development/Libraries
Requires: %{name} = %{version}

%description file
Ecore File Library

%if %{with lib_ecore_imf}
%package imf
Summary: Ecore IMF functions
Group: Development/Libraries
Requires: %{name} = %{version}
%description imf
Ecore IMF functions
%endif

%package input
Summary: Ecore input functions
Group: Development/Libraries
Requires: %{name} = %{version}

%description input
Ecore input functions

%package ipc
Summary: Ecore inter-process communication functions
Group: Development/Libraries
Requires: %{name} = %{version}

%description ipc
Ecore inter-process communication functions

%package x
Summary: Ecore functions for dealing with the X Windows System
Group: Development/Libraries
Requires: %{name} = %{version}

%description x
Ecore functions for dealing with the X Windows System

%prep
rm -rf "$RPM_BUILD_ROOT"
%setup -q

%build
touch config.rpath
./autogen.sh --prefix=/usr                                                     \
             --libdir=/usr/lib64

%install
make %{?_smp_mflags}
make doc
%makeinstall

%clean
rm -rf "$RPM_BUILD_ROOT"

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%defattr(-, root, root)
%doc AUTHORS COPYING* README*
%{_libdir}/libecore*.so.*
%{_datadir}/locale/*/LC_MESSAGES/ecore.mo

%files devel
%defattr(-, root, root)
%doc doc/html
%{_libdir}/*.so
#%{_libdir}/ecore/immodules/*.so
#%{_libdir}/ecore/immodules/*.la
%{_libdir}/*.la
%{_libdir}/*.a
%{_libdir}/pkgconfig/*
#%{_datadir}/aclocal/*
%{_includedir}/ecore-1/*.h

%files con
%defattr(-, root, root)
%{_libdir}/libecore_con*.so.*

%if %{with lib_ecore_directfb}
%files directfb
%defattr(-, root, root)
%{_libdir}/libecore_directfb*.so.*
%endif

%if %{with lib_ecore_evas}
%files evas
%defattr(-, root, root)
%{_libdir}/libecore_evas*.so.*
%endif

%if %{with lib_ecore_fb}
%files fb
%defattr(-, root, root)
%{_libdir}/libecore_fb*.so.*
%endif

%files file
%defattr(-, root, root)
%{_libdir}/libecore_file*.so.*

%if %{with lib_ecore_imf}
%files imf
%defattr(-, root, root)
%{_libdir}/libecore_imf*.so.*
%endif

%files input
%defattr(-, root, root)
%{_libdir}/libecore_input*.so.*

%files ipc
%defattr(-, root, root)
%{_libdir}/libecore_ipc*.so.*

%if %{with lib_ecore_x}
%files x
%defattr(-, root, root)
%{_libdir}/libecore_x*.so.*
%endif

