CMAKE_BUILD_DIR=build

run: build
	$(CMAKE_BUILD_DIR)/src/mpzd

build:
	mkdir -p $(CMAKE_BUILD_DIR)
	cmake -S . -B $(CMAKE_BUILD_DIR)
	cmake --build $(CMAKE_BUILD_DIR)

clean:
	rm -rf $(CMAKE_BUILD_DIR)


.PHONY: build run clean
