# Package Makefile for httpserver utility

PKG_NAME := httpserver
PKG_DIR  := $(CURDIR)

SRCS     := $(shell find $(PKG_DIR) -name '*.c' | sort)
REL_SRCS := $(patsubst $(PKG_DIR)/%,%,$(SRCS))
OBJS     := $(patsubst %.c,$(OBJ_DIR)/$(PKG_NAME)/%.o,$(REL_SRCS))
BINS     := $(patsubst $(OBJ_DIR)/$(PKG_NAME)/%.o,$(BIN_DIR)/$(PKG_NAME)/%,$(OBJS))

.PHONY: all install clean

all: $(BINS)

$(OBJ_DIR)/$(PKG_NAME)/%.o: $(PKG_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c -o $@ $<

$(BIN_DIR)/$(PKG_NAME)/%: $(OBJ_DIR)/$(PKG_NAME)/%.o
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install:
	@for b in $(BINS); do \
	    dest_name=$${b##*/}; \
	    echo "  Install [$(PKG_NAME)] $$b -> $(ROOTFS_DIR)/Server/httpserver/bin/$$dest_name"; \
	    mkdir -p $(ROOTFS_DIR)/Server/httpserver/bin; \
	    cp "$$b" "$(ROOTFS_DIR)/Server/httpserver/bin/$$dest_name"; \
	done
	@mkdir -p $(ROOTFS_DIR)/Server/httpserver/www
	@cp -r $(PKG_DIR)/www/* $(ROOTFS_DIR)/Server/httpserver/www/

clean:
	@rm -rf $(OBJ_DIR)/$(PKG_NAME) $(BIN_DIR)/$(PKG_NAME)

-include $(OBJS:.o=.d)
