BIN := netparent-web
HASHPW := hashpw

GO ?= go
LDFLAGS ?= -s -w

.PHONY: all build hashpw run clean tidy

all: build hashpw

build:
	$(GO) build -ldflags '$(LDFLAGS)' -o $(BIN) ./cmd/netparent-web

hashpw:
	$(GO) build -ldflags '$(LDFLAGS)' -o $(HASHPW) ./cmd/hashpw

run: build
	./$(BIN)

tidy:
	$(GO) mod tidy

clean:
	rm -f $(BIN) $(HASHPW)
