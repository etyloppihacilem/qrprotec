from django.contrib import admin

from .models import (
    Items, ItemsPacks, ItemType, LotRequirements, Lots, LotType, SealedPacks, Secouristes, VerifItem, Verifs,
)


@admin.register(ItemType)
class ItemTypeAdmin(admin.ModelAdmin):
    list_display = ('type', 'name', 'min_quantity', 'perissable')


@admin.register(Items)
class ItemsAdmin(admin.ModelAdmin):
    list_display = ('iid', 'pack', 'location', 'status', 'last_seen')
    list_filter = ('status',)
    search_fields = ('iid',)


class LotRequirementsInline(admin.TabularInline):
    model = LotRequirements


@admin.register(LotType)
class LotTypeAdmin(admin.ModelAdmin):
    list_display = ('type', 'name', 'version')
    inlines = [LotRequirementsInline]


@admin.register(Lots)
class LotsAdmin(admin.ModelAdmin):
    list_display = ('id', 'name', 'lot_type', 'last_verif', 'active')


@admin.register(Secouristes)
class SecouristesAdmin(admin.ModelAdmin):
    list_display = ('matricule', 'nom', 'prenom', 'privileged', 'active', 'key_expires')


admin.site.register(ItemsPacks)
admin.site.register(SealedPacks)
admin.site.register(Verifs)
admin.site.register(VerifItem)
