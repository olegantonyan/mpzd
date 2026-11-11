HOST=localhost
PORT=6601
LIBRARY_DIR=/run/media/oleg/c3996ce0-a379-4403-9d64-7d4c0536463f/music
MPZD=printf '%s\n' $(1) | ncat --idle-timeout 1 $(HOST) $(PORT)

status:
	$(call MPZD,status)

libraries:
	$(call MPZD,libraries)

ls:
	$(call MPZD,'ls "$(LIBRARY_DIR)"')

playlists:
	$(call MPZD,playlists)

play:
	$(call MPZD,play)

pause:
	$(call MPZD,pause)

stop:
	$(call MPZD,stop)

next:
	$(call MPZD,next)

prev:
	$(call MPZD,prev)

watch:
	(echo 'watch on'; cat) | ncat $(HOST) $(PORT)

.PHONY: status libraries ls playlists play pause stop next prev watch
