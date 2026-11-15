HOST=localhost
PORT=6601
MPZD=printf '%s\n' $(1) | ncat --idle-timeout 1 $(HOST) $(PORT)

status:
	$(call MPZD,status)

library:
	$(call MPZD,library)

ls:
	$(call MPZD,ls)

outputs:
	$(call MPZD,outputs)

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

.PHONY: status library ls outputs playlists play pause stop next prev watch
