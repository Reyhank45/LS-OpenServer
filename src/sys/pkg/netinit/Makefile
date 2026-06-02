PKG_NAME := $(notdir $(CURDIR))
PKG_DIR  := $(CURDIR)

SRCS     := $(shell find $(PKG_DIR) -name '*.c' | sort)
REL_SRCS := $(patsubst $(PKG_DIR)/%,%,$(SRCS))
OBJS     := $(patsubst %.c,$(OBJ_DIR)/$(PKG_NAME)/%.o,$(REL_SRCS))
BINS     := $(patsubst $(OBJ_DIR)/$(PKG_NAME)/%.o,$(BIN_DIR)/$(PKG_NAME)/%,$(OBJS))

.PHONY: all install clean

all: $(BINS)

$(OBJ_DIR)/$(PKG_NAME)/%.o: $(PKG_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  Compile [$(CC)] [$(PKG_NAME)] $< -> $@"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(BIN_DIR)/$(PKG_NAME)/%: $(OBJ_DIR)/$(PKG_NAME)/%.o
	@mkdir -p $(dir $@)
	@echo "  Link    [$(CC)] [$(PKG_NAME)] $< -> $@"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install:
	@for b in $(BINS); do \
	    dest_name=$$(echo "$$b" | sed 's|$(BIN_DIR)/$(PKG_NAME)/||'); \
	    echo "  Install [$(PKG_NAME)] $$b -> $(ROOTFS_DIR)/System/$$dest_name"; \
	    cp "$$b" "$(ROOTFS_DIR)/System/$$dest_name"; \
	done

clean:
	@echo "  Clean   [$(PKG_NAME)]"
	@rm -rf $(OBJ_DIR)/$(PKG_NAME) $(BIN_DIR)/$(PKG_NAME)

-include $(OBJS:.o=.d)
