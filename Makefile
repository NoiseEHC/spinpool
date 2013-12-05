.PHONY: clean All

All:
	@echo "----------Building project:[ spinpool - Debug ]----------"
	@$(MAKE) -f  "spinpool.mk"
clean:
	@echo "----------Cleaning project:[ spinpool - Debug ]----------"
	@$(MAKE) -f  "spinpool.mk" clean
