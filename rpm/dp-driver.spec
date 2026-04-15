%{?!package_name: %{error: You did not specify a package name (%%package_name)}}
%{?!version: %{error: You did not specify a module version (%%version)}}
%{?!kernel_versions: %{error: You did not specify kernel versions (%%kernel_version)}}
%{?!packager: %define packager DKMS <dkms-devel@lists.us.dell.com>}
%{?!license: %define license Unknown}
%{?!_dkmsdir: %define _dkmsdir /var/lib/dkms}
%{?!_srcdir: %define _srcdir %_prefix/src}
%{?!_datarootdir: %define _datarootdir %{_datadir}}

Summary:           %{package_name} %{version} dkms package
Name:              %{package_name}
Version:           %{version}
License:           %license
Release:           1dkms
BuildArch:         x86_64
Requires(post):    dkms >= 3.0.6
Requires:          mhi-host = %{version}
BuildRoot:         %{_tmppath}/%{name}-%{version}-%{release}-root/

%description
Kernel modules for %{package_name} %{version} in a DKMS wrapper.

%prep

%install
echo -e "\n About to start INSTALL step.. \n"
if [ "$RPM_BUILD_ROOT" != "/" ]; then
        rm -rf $RPM_BUILD_ROOT
fi
mkdir -p $RPM_BUILD_ROOT/%{_srcdir}
mkdir -p $RPM_BUILD_ROOT/%{_datarootdir}/%{package_name}

if [ -d %{_sourcedir}/%{package_name}-%{version} ]; then
        cp -Lpr %{_sourcedir}/%{package_name}-%{version} $RPM_BUILD_ROOT/%{_srcdir}
fi

if [ -f %{package_name}-%{version}.dkms.tar.gz ]; then
        install -m 644 %{package_name}-%{version}.dkms.tar.gz $RPM_BUILD_ROOT/%{_datarootdir}/%{package_name}
fi

if [ -f %{_sourcedir}/common.postinst ]; then
        install -m 755 %{_sourcedir}/common.postinst $RPM_BUILD_ROOT/%{_datarootdir}/%{package_name}/postinst
fi

mkdir -p $RPM_BUILD_ROOT%{_includedir}/linux
install -m 644 %{_sourcedir}/%{package_name}-%{version}/dp-driver/include/uapi/linux/csm_dp_ioctl.h $RPM_BUILD_ROOT%{_includedir}/linux

mkdir -p %{buildroot}%{_sysconfdir}/modules-load.d
install -m 644 %{_sourcedir}/%{package_name}-%{version}/dp-driver/modules-load.d/dp-driver.conf %{buildroot}%{_sysconfdir}/modules-load.d/dp-driver.conf

mkdir -p %{buildroot}%{_sysconfdir}/modprobe.d
install -m 644 %{_sourcedir}/%{package_name}-%{version}/dp-driver/modprobe.d/dp-driver.conf %{buildroot}%{_sysconfdir}/modprobe.d/dp-driver.conf

%clean
if [ "$RPM_BUILD_ROOT" != "/" ]; then
        rm -rf $RPM_BUILD_ROOT
fi

%post

for POSTINST in %{_prefix}/lib/dkms/common.postinst %{_datarootdir}/%{package_name}/postinst; do
        if [ -f $POSTINST ]; then
                $POSTINST %{package_name} %{version} %{_datarootdir}/%{package_name}
                exit $?
        fi
        echo "WARNING: $POSTINST does not exist."
done
echo -e "ERROR: DKMS version is too old and %{package_name} was not"
echo -e "built with legacy DKMS support."
echo -e "You must either rebuild %{package_name} with legacy postinst"
echo -e "support or upgrade DKMS to a more current version."
exit 1

%preun
echo -e
echo -e "Uninstall of %{package_name} module (version %{version}) beginning:"
dkms remove -m %{package_name} -v %{version} --all --rpm_safe_upgrade
exit 0

%files
%defattr(-,root,root)
%{_srcdir}
%{_datarootdir}/%{package_name}/
%{_includedir}/linux/csm_dp_ioctl.h
%config %{_sysconfdir}/modules-load.d/dp-driver.conf
%config %{_sysconfdir}/modprobe.d/dp-driver.conf

%changelog
* %(date "+%a %b %d %Y") %packager %{version}-%{release}
- Automatic build by DKMS
