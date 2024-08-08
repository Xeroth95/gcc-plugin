PLUGIN_SOURCE_FILES= plugin.cc

CXXFLAGS += -ggdb3 -Wall -Wextra

plugin.so:HOST_GCC=g++
plugin.so:TARGET_GCC=gcc
plugin.so:GCCPLUGINS_DIR:= $(shell $(TARGET_GCC) -print-file-name=plugin)
plugin.so:CXXFLAGS+= -I$(GCCPLUGINS_DIR)/include -fPIC -fno-rtti -O2
plugin.so: $(PLUGIN_SOURCE_FILES)
	$(HOST_GCC) -shared $(CXXFLAGS) $^ -o $@

test: plugin.so
	gcc -fplugin=./$^ example.c

dump:
	rm -rf dump
	mkdir dump
	cd dump && gcc ../example.c -fdump-tree-all

.PHONY: test dump
