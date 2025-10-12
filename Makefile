CMAKE_BUILD_DIR=build

run: build
	$(CMAKE_BUILD_DIR)/src/mpzd

build:
	mkdir -p $(CMAKE_BUILD_DIR)
	cmake -S . -B $(CMAKE_BUILD_DIR)
	cmake --build $(CMAKE_BUILD_DIR)

clean:
	rm -rf $(CMAKE_BUILD_DIR)

######

play:
	echo '{"cmd":"play","filepath":"/run/media/oleg/c3996ce0-a379-4403-9d64-7d4c0536463f/music/The_Kovenant_-_Fragments_of_Reality_(Sample)_-_(mp3poisk.net).mp3"}' | ncat localhost 6601

stop:
	echo '{"cmd":"stop"}' | ncat localhost 6601

pause:
	echo '{"cmd":"pause"}' | ncat localhost 6601

unpause:
	echo '{"cmd":"unpause"}' | ncat localhost 6601

get_state:
		echo '{"cmd":"get_state"}' | ncat localhost 6601

.PHONY: build run clean
