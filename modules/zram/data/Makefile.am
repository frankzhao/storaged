
NULL = 

polkitdir       = $(datadir)/polkit-1/actions
polkit_in_files = org.storaged.Storaged.zram.policy.in
polkit_DATA     = $(polkit_in_files:.policy.in=.policy)

@INTLTOOL_POLICY_RULE@

EXTRA_DIST =                                                    \
	org.storaged.Storaged.zram.xml                          \
	$(polkit_in_files)                                      \
	$(NULL)

clean-local:
	rm -f *~ $(polkit_DATA)

