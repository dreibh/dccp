EXTRA_DIST	= include net
MODULES		= dccp dccp_diag

all:
	$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR)/net/dccp modules \
		ROOTDIR=$(CURDIR) \
		CONFIG_IP_DCCP=m \
		CONFIG_INET_DCCP_DIAG=n \
		CONFIG_IP_DCCP_CCID2_DEBUG=n \
		CONFIG_IP_DCCP_CCID3=m \
		CONFIG_IP_DCCP_CCID3_DEBUG=n \
		CONFIG_IP_DCCP_TFRC_LIB=m \
		CONFIG_IP_DCCP_TFRC_DEBUG=n

install: uninstall_modules install_headers install_modules depmod

uninstall: uninstall_modules uninstall_headers depmod

clean:
	$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR)/net/dccp clean

install_headers:
	install -m 644 include/uapi/linux/dccp.h /usr/include/linux

install_modules: all $(KERNEL_EXTRA)
	$(foreach module, $(MODULES), \
		! [ -f net/dccp/$(module).ko ] || \
		install -m 644 net/dccp/$(module).ko $(KERNEL_EXTRA);)

uninstall_modules:
	$(foreach module, $(MODULES), \
		! [ -d /sys/module/$(module) ] || rmmod $(module); \
		rm -f $(KERNEL_EXTRA)/$(module).ko;)

uninstall_headers:
	rm -f /usr/include/linux/dccp.h

$(KERNEL_EXTRA):
	mkdir -p $@

depmod:
	depmod -a

MAINTAINERCLEANFILES	= Makefile.in
