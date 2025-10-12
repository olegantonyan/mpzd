HOST=localhost
PORT=6601
LIBRARY_DIR=/run/media/oleg/c3996ce0-a379-4403-9d64-7d4c0536463f/music
TEST_FILE=The_Kovenant_-_Fragments_of_Reality_(Sample)_-_(mp3poisk.net).mp3

info:
	echo '{"cmd":"info"}' | ncat localhost 6601

play:
	echo '{"cmd":"play","filepath":"$(LIBRARY_DIR)/$(TEST_FILE)"}' | ncat $(HOST) $(PORT)

stop:
	echo '{"cmd":"stop"}' | ncat $(HOST) $(PORT)

pause:
	echo '{"cmd":"pause"}' | ncat $(HOST) $(PORT)

unpause:
	echo '{"cmd":"unpause"}' | ncat $(HOST) $(PORT)

get_state:
		echo '{"cmd":"get_state"}' | ncat $(HOST) $(PORT)

ls:
	echo '{"cmd":"ls","path":"$(LIBRARY_DIR)"}' | $(HOST) $(PORT)

.PHONY: info play stop pause unpause get_state ls
