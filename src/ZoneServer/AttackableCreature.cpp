/*
---------------------------------------------------------------------------------------
This source file is part of swgANH (Star Wars Galaxies - A New Hope - Server Emulator)
For more information, see http://www.swganh.org


Copyright (c) 2006 - 2010 The swgANH Team

---------------------------------------------------------------------------------------
*/

#include "AttackableCreature.h"
#include "CellObject.h"
#include "Inventory.h"
#include "LairObject.h"
#include "MessageLib/MessageLib.h"
#include "NpcManager.h"
#include "PlayerObject.h"
#include "QuadTree.h"
#include "ResourceContainer.h"
#include "Weapon.h"
#include "WorldManager.h"
#include "ScoutManager.h"
#include "WorldConfig.h"
#include "ZoneTree.h"
#include "ZoneServer/NonPersistentNpcFactory.h"
#include "utils/rand.h"

// TODO: Implement by functionality.
static const int64 readyDefaultPeriodTime = 1000;
static const int64 activeDefaultPeriodTime = 500;

// For test.
static int64 gCreatureSpawnCounter = 0;
static int64 gCreatureDeathCounter = 0;


//=============================================================================

AttackableCreature::AttackableCreature(uint64 templateId)
: NPCObject()
, mLairId(0)
, mCombatState(State_Unspawned)
, mPrimaryWeapon(NULL)
, mSecondaryWeapon(NULL)
, mReadyDelay(0)
, mCombatTimer(0)
, mRoamingSteps(-1)
, mAssistanceNeededWithId(0)
, mAssistedTargetId(0)
, mLairNeedAssistanceWithId(0)
, mAttackTauntSent(false)
, mHoming(false)
, mIsAssistingLair(false)
, mWarningTauntSent(false)
{
	mNpcFamily	= NpcFamily_AttackableCreatures;
	// mNpcTemplateId = templateId;
	this->setTemplateId(templateId);

	mRadialMenu = RadialMenuPtr(new RadialMenu());
	mRadialMenu->addItem(1,0,radId_combatAttack,radAction_Default);
	mRadialMenu->addItem(2,0,radId_examine,radAction_Default);
}

//=============================================================================

AttackableCreature::~AttackableCreature()
{
	mRadialMenu.reset();
	// mDamageDealers.clear();

	delete mPrimaryWeapon;
	delete mSecondaryWeapon;
	// delete mSpawn;
}

//=============================================================================


void AttackableCreature::prepareCustomRadialMenu(CreatureObject* creatureObject, uint8 itemCount)
{
	mRadialMenu.reset();
	mRadialMenu = RadialMenuPtr(new RadialMenu());

	if (this->isDead())
	{
		if (!creatureObject->isDead() && !creatureObject->isIncapacitated())
		{
			// mRadialMenu = RadialMenuPtr(new RadialMenu());
			mRadialMenu->addItem(1,0,radId_lootAll,radAction_ObjCallback, "@ui_radial:loot_all");
			mRadialMenu->addItem(2,1,radId_loot,radAction_ObjCallback, "@ui_radial:loot");

			//Harvesting of Corpse :D
			if(creatureObject->checkSkill(31) && this->allowedToLoot(creatureObject->getId(), creatureObject->getGroupId()))
			{
				mRadialMenu->addItem(4,0,radId_serverHarvestCorpse, radAction_ObjCallback, "@sui:harvest_corpse");

				if(this->hasAttribute("res_meat"))
					mRadialMenu->addItem(5,3,radId_diceRoll, radAction_ObjCallback, "@sui:harvest_meat");

				if(this->hasAttribute("res_hide"))
					mRadialMenu->addItem(6,3,radId_diceTwoFace, radAction_ObjCallback, "@sui:harvest_hide");

				if(this->hasAttribute("res_bone"))
					mRadialMenu->addItem(7,3,radId_diceThreeFace, radAction_ObjCallback, "@sui:harvest_bone");
			}
		}
	}
	else
	{
		//if(creatureObject->checkSkill(31) /*&& this->hasAttribute("res_milk")*/ && !creatureObject->checkState(CreatureState_Combat))
			//mRadialMenu->addItem(5,0,radId_serverMenu5, radAction_ObjCallback, "Collect Milk");
	}
}

//=============================================================================

void AttackableCreature::handleObjectMenuSelect(uint8 messageType,Object* srcObject)
{
	if (PlayerObject* playerObject = dynamic_cast<PlayerObject*>(srcObject))
	{
		switch(messageType)
		{
			case radId_lootAll:
			{
				// First, we have to have a connected player..
				// if (playerObject->isConnected() && !playerObject->isDead() && !playerObject->isIncapacitated() && this->isDead())
				{
					playerObject->getController()->lootAll(this->getId(),playerObject);
				}
			}
			break;

			case radId_loot:
			{
				// First, we have to have a connected player..
				if (playerObject->isConnected() && !playerObject->isDead() && !playerObject->isIncapacitated() && this->isDead())
				{
					if (this->allowedToLoot(playerObject->getId(), playerObject->getGroupId()))
					{
						Inventory* inventory = dynamic_cast<Inventory*>(this->getEquipManager()->getEquippedObject(CreatureEquipSlot_Inventory));
						if (inventory)
						{
							// Open the Inventory.
							ObjectIDList*			objList				= inventory->getObjects();
							ObjectIDList::iterator	containerObjectIt	= objList->begin();

							while (containerObjectIt != objList->end())
							{
								Object* object = gWorldManager->getObjectById((*containerObjectIt));

								if (TangibleObject* tangibleObject = dynamic_cast<TangibleObject*>(object))
								{
									// reminder: objects are owned by the global map, containers only keeps references
									// send the creates, if we are not owned by any player OR by exactly this player.
									if (playerObject)
									{
										if (!object->getPrivateOwner() || (object->isOwnedBy(playerObject)))
										{
											// could be a resource container, need to check this first, since it inherits from tangible
											if (ResourceContainer* resCont = dynamic_cast<ResourceContainer*>(object))
											{
												gMessageLib->sendCreateResourceContainer(resCont,playerObject);
											}
											// or a tangible
											else
											{
												gMessageLib->sendCreateTangible(tangibleObject,playerObject);
											}
										}
									}
								}
								++containerObjectIt;
							}
							gMessageLib->sendOpenedContainer(this->getId()+1, playerObject);
							// gMessageLib->sendOpenedContainer(this->getId(), playerObject);

							int32 lootedCredits = inventory->getCredits();
							inventory->setCredits(0);
							if (lootedCredits > 0)
							{
								if (playerObject->getGroupId() != 0)
								{
									// We are grouped. Split the credits with the group members in range.
									PlayerList inRangeMembers = playerObject->getInRangeGroupMembers(false);

									// Number of additional members.
									int32 noOfMembers = inRangeMembers.size();
									int32 splittedCredits = lootedCredits/(noOfMembers + 1);

									int8 str[64];
									sprintf(str,"%u", lootedCredits);
									string lootCreditsString(str);
									lootCreditsString.convert(BSTRType_Unicode16);

									if (splittedCredits == 0)
									{
										// To little to split.
										// "GROUP] You split %TU credits and receive %TT credits as your share."
										gMessageLib->sendSystemMessage(playerObject, L"", "group", "prose_split_coins_self", "", "", L"", 0, "", "", lootCreditsString, 0, 0, 0, "", "", lootCreditsString);
										// "There are insufficient group funds to split"
										gMessageLib->sendSystemMessage(playerObject, L"", "error_message", "nsf_to_split");
									}
									else
									{
										int32 totalProse = lootedCredits;
										PlayerList::iterator it	= inRangeMembers.begin();
										while (it != inRangeMembers.end())
										{
											// "[GROUP] You receive %DI credits as your share."
											gMessageLib->sendSystemMessage((*it), L"", "group", "prose_split", "", "", L"", splittedCredits);

											// Now we need to add the credits to player inventory.
											Inventory* playerInventory = dynamic_cast<Inventory*>((*it)->getEquipManager()->getEquippedObject(CreatureEquipSlot_Inventory));
											if (playerInventory)
											{
												playerInventory->updateCredits(splittedCredits);
											}
											totalProse -= splittedCredits;
											++it;
										}

										int8 str[64];
										sprintf(str,"%u", totalProse);
										string splitedLootCreditsString(str);
										splitedLootCreditsString.convert(BSTRType_Unicode16);

										// "GROUP] You split %TU credits and receive %TT credits as your share."
										gMessageLib->sendSystemMessage(playerObject, L"", "group", "prose_split_coins_self", "", "", L"", 0, "", "", splitedLootCreditsString, 0, 0, 0, "", "", lootCreditsString);

										// Now we need to add the credits to our own inventory.
										Inventory* playerInventory = dynamic_cast<Inventory*>(playerObject->getEquipManager()->getEquippedObject(CreatureEquipSlot_Inventory));
										if (playerInventory)
										{
											playerInventory->updateCredits(totalProse);
										}
									}
								}
								else
								{
									// We looted some credits, always a start.
									// int8 str[128];
									// sprintf(str,"%u credits", lootedCredits);
									// string lootCreditsString(str);
									// lootCreditsString.convert(BSTRType_Unicode16);
									// gMessageLib->sendSystemMessage(playerObject, L"", "spam", "loot_item_self", "", "", L"", 0, getSpeciesGroup(), getSpeciesString(), L"", 0, 0, 0, "", "", lootCreditsString);
									gMessageLib->sendSystemMessage(playerObject, L"", "base_player", "prose_coin_loot", "", "", L"", lootedCredits, getSpeciesGroup(), getSpeciesString());

									// Now we need to add the credits to our own inventory.
									Inventory* playerInventory = dynamic_cast<Inventory*>(playerObject->getEquipManager()->getEquippedObject(CreatureEquipSlot_Inventory));
									if (playerInventory)
									{
										playerInventory->updateCredits(lootedCredits);
									}
								}
							}

							// @error_message:corpse_empty "You find nothing else of value on the selected corpse."
							if (objList->size() == 0)
							{
								if (lootedCredits == 0)
								{
									// There was no credits and no items in the inventory.
									gMessageLib->sendSystemMessage(playerObject, L"", "error_message", "corpse_empty");
								}

								// Put this creaure in the pool of delayed destruction and remove the corpse from scene.
								gWorldManager->addCreatureObjectForTimedDeletion(this->getId(), LootedCorpseTimeout);
							}
						}
					}
					else
					{
						// Player do not have permission to loot this corpse.
						gMessageLib->sendSystemMessage(playerObject,L"","error_message","no_corpse_permission");
					}
				}
			}
			break;

			case radId_serverHarvestCorpse: //NORMAL HARVEST CORPSE!
				gScoutManager->handleHarvestCorpse(playerObject, this, HARVEST_ANY);
				break;
			case radId_diceRoll: //HARVESTING OF MEAT!
				gScoutManager->handleHarvestCorpse(playerObject, this, HARVEST_MEAT);
				break;
			case radId_diceTwoFace: //HARVESTING OF HIDE!
				gScoutManager->handleHarvestCorpse(playerObject, this, HARVEST_HIDE);
				break;
			case radId_diceThreeFace: //HARVESTING OF BONE!
				gScoutManager->handleHarvestCorpse(playerObject, this, HARVEST_BONE);
				break;
			case radId_serverMenu5: //MILKING!
				gMessageLib->sendSystemMessage(playerObject,L"YOU TRIED TO MILK ME! WHY I OUTTA!");
				break;

			default:
			{
				gLogger->log(LogManager::NOTICE,"AttackableCreature::handleObjectMenuSelect Unhandled messageType = %u\n", messageType);
			}
			break;

		}
	}
}

//=============================================================================
//
//	Routines realted to NPC AI
//
//=============================================================================

void AttackableCreature::addKnownObject(Object* object)
{
	if(checkKnownObjects(object))
	{
		gLogger->log(LogManager::NOTICE,"AttackableCreature::addKnownObject %I64u couldnt be added to %I64u already in it", object->getId(), this->getId());
		return;
	}

	if (object->getType() == ObjType_Player)
	{
		mKnownPlayers.insert(dynamic_cast<PlayerObject*>(object));

		if ((this->getAiState() == NpcIsDormant) && this->isAgressive() && isSpawned())	// Do not wake up the not spawned.
		{
			gWorldManager->forceHandlingOfDormantNpc(this->getId());
		}
	}
	else
	{
		mKnownObjects.insert(object);
	}
}


//=============================================================================
//
//	Set new active target, if any in range.
//  Object already in defender list is not set. hmmm
//
//	Taunt message or flytext "!" when attempting an attack, will only be shown once.
//  Peace will enable taunt/flytext again.
//
//
//	Return true if target set.
//

bool AttackableCreature::setTargetInAttackRange(void)
{
	bool targetSet = false;

	// Attack nearest target or the first target found within range?
	// if (mAgressiveMode)
	if (isAgressive())
	{
		PlayerObjectSet* knownPlayers = this->getKnownPlayers();
		PlayerObjectSet::iterator it = knownPlayers->begin();

		// For now, we attack the first target we see, that have we enough aggro towards.
		while(it != knownPlayers->end())
		{
			if (!(*it)->isIncapacitated() && !(*it)->isDead())
			{
				// We only accepts new targets.
				/*
				ObjectIDList::iterator defenderIt = this->getDefenders()->begin();
				bool newTarget = true;
				while (defenderIt != this->getDefenders()->end())
				{
					if ((*defenderIt) == (*it)->getId())
					{
						// Already in my list, try next.
						newTarget = false;
						break;
					}
					defenderIt++;
				}
				*/
				// Only test players not having aggro.

				if ((!this->attackerHaveAggro((*it)->getId())) && gWorldManager->objectsInRange(this->getId(), (*it)->getId(), this->getAttackRange()))
				{
					if (gWorldConfig->isInstance())
					{
						if (this->getPrivateOwner() == (*it)->getId())
						{
						}
						else
						{
							gLogger->log(LogManager::DEBUG,"Attacking WRONG TARGET = %s\n",(*it)->getFirstName().getAnsi());
						}
					}
					// We have a new target in range etc.. But we may need him to be visible for a while before we attack.
					this->updateAggro((*it)->getId(), (*it)->getGroupId(), (*it)->getPosture());

					if (!targetSet)
					{
						if (this->attackerHaveAggro((*it)->getId()))
						{
							this->setTarget((*it)->getId());
							targetSet = true;
							// TEST ERU gMessageLib->sendTargetUpdateDeltasCreo6(this);

							// No break, cycle all targets in range, since we are building up aggro.
							// break;
						}
					}
				}
			}
			else
			{
				// Handle incapped and dead targets, for now we remove any aggro to them.
				this->updateAggro((*it)->getId(), (*it)->getGroupId(), (*it)->getPosture());


				// Let's this player rebuild is aggro before we attack him again.
				if (this->mLairNeedAssistanceWithId == (*it)->getId())
				{
					this->mAssistanceNeededWithId = 0;
					this->mLairNeedAssistanceWithId = 0;
					this->mIsAssistingLair = false;
				}
			}
			++it;
		}
	}

	if (targetSet && !this->isAttackTauntSent())
	{
		this->setAttackTauntSent();

		if (this->getAttackStartMessage().getLength())
		{
			// for now, let's just taunt him.
			string msg(this->getAttackStartMessage());
			msg.convert(BSTRType_Unicode16);
			char quack[5][32];
			memset(quack, 0, sizeof(quack));

			if (!gWorldConfig->isInstance())
			{
				gMessageLib->sendSpatialChat(this, msg, quack);
				// gMessageLib->sendCreatureAnimation(this,gWorldManager->getNpcConverseAnimation(27));	// poke
			}
			else
			{
				PlayerObject* playerObject = dynamic_cast<PlayerObject*>(this->getTarget());
				if (playerObject)
				{
					gMessageLib->sendSpatialChat(this, msg, quack, playerObject);
					// gMessageLib->sendCreatureAnimation(this,gWorldManager->getNpcConverseAnimation(27), playerObject);
				}
			}
		}
		else
		{
			// Let's play with the flytexts.
			gMessageLib->sendFlyText(this,"npc_reaction/flytext","threaten",255,0,0);
		}
	}
	return targetSet;
}

//=============================================================================
//
//	Show warning message when target gets near this npc.
//	For now a spatial chat used in the tutorial, but it can easily be customised to other visuals,
//	like the red "?" you see when you approach a creature.

bool AttackableCreature::showWarningInRange(void)
{
	bool targetSet = false;

	// Attack nearest target or the first target found within range?
	if (isAgressive())
	{
		PlayerObjectSet* knownPlayers = this->getKnownPlayers();
		PlayerObjectSet::iterator it = knownPlayers->begin();

		// For now, we attack the first target we see.
		bool isOldTarget = false;
		while(it != knownPlayers->end())
		{
			if (!(*it)->isIncapacitated() && !(*it)->isDead())
			{
				ObjectIDList::iterator defenderIt = this->getDefenders()->begin();
				bool newTarget = true;
				while (defenderIt != this->getDefenders()->end())
				{
					if ((*defenderIt) == (*it)->getId())
					{
						// Already in my list, try next.
						newTarget = false;
						break;
					}
					defenderIt++;
				}

				// We only accepts new targets.
				// if ((!this->getTarget() || ((*it) != this->getTarget())) &&
				// 	(newTarget && gWorldManager->objectsInRange(this->getId(), (*it)->getId(), this->getAttackWarningRange())))
				if (newTarget && gWorldManager->objectsInRange(this->getId(), (*it)->getId(), this->getAttackWarningRange()))
				{
					if (!this->getTarget() || ((*it) != this->getTarget()))
					{
						this->setTarget((*it)->getId());
						targetSet = true;
						// TEST ERU gMessageLib->sendTargetUpdateDeltasCreo6(this);
						break;
					}
					else
					{
						// same old fellow targeted.
						isOldTarget = true;
					}
				}
			}
			++it;
		}
		if (!targetSet && !isOldTarget)
		{
			// We have a no targets in range, enable taunts.
			this->clearWarningTauntSent();
		}
	}

	if (targetSet && !this->isWarningTauntSent())
	{
		this->setWarningTauntSent();

		if (getAttackWarningMessage().getLength())
		{
			string msg(getAttackWarningMessage());
			msg.convert(BSTRType_Unicode16);
			char quack[5][32];
			memset(quack, 0, sizeof(quack));

			if (!gWorldConfig->isInstance())
			{
				gMessageLib->sendSpatialChat(this, msg, quack);
				// gMessageLib->sendCreatureAnimation(this,gWorldManager->getNpcConverseAnimation(27));	// poke
			}
			else
			{
				PlayerObject* playerObject = dynamic_cast<PlayerObject*>(this->getTarget());
				if (playerObject)
				{
					gMessageLib->sendSpatialChat(this, msg, quack, playerObject);
					// gMessageLib->sendCreatureAnimation(this,gWorldManager->getNpcConverseAnimation(27), playerObject);
				}
			}
		}
		else
		{
			// Let's play with the flytexts.
			gMessageLib->sendFlyText(this,"npc_reaction/flytext","alert",255,0,0);
		}
	}
	return targetSet;
}



//=============================================================================
//
//	Target defender if within max weapon range.
//	Return true if target set.
//

bool AttackableCreature::setTargetDefenderWithinWeaponRange(void)
{
	bool foundTarget = false;

	ObjectIDList::iterator defenderIt = this->getDefenders()->begin();
	while (defenderIt != this->getDefenders()->end())
	{
		if (CreatureObject* defenderCreature = dynamic_cast<CreatureObject*>(gWorldManager->getObjectById((*defenderIt))))
		{
			if (!defenderCreature->isIncapacitated() && !defenderCreature->isDead())
			{
				if (gWorldManager->objectsInRange(this->getId(), *defenderIt, this->getWeaponMaxRange()))
				{
					// Do only attack objects that have build up enough aggro.
					if (this->attackerHaveAggro(defenderCreature->getId()))
					{
						if (this->getTargetId() != *defenderIt)
						{
							this->setTarget(*defenderIt);
							// TEST ERU gMessageLib->sendTargetUpdateDeltasCreo6(this);
						}
						foundTarget = true;
						break;
					}
				}
			}
		}
		++defenderIt;
	}

	if (foundTarget)
	{
		if (getAttackedMessage().getLength())
		{
			// for now, let's just taunt him.
			string msg(getAttackedMessage());
			msg.convert(BSTRType_Unicode16);
			char quack[5][32];
			memset(quack, 0, sizeof(quack));

			if (!gWorldConfig->isInstance())
			{
				gMessageLib->sendSpatialChat(this, msg, quack);
				// gMessageLib->sendCreatureAnimation(this,gWorldManager->getNpcConverseAnimation(27));	// poke
			}
			else
			{
				PlayerObject* playerObject = dynamic_cast<PlayerObject*>(this->getTarget());
				if (playerObject)
				{
					gMessageLib->sendSpatialChat(this, msg, quack, playerObject);
					// gMessageLib->sendCreatureAnimation(this,gWorldManager->getNpcConverseAnimation(27), playerObject);
				}
			}
		}
	}
	return foundTarget;
}


//=============================================================================
//
//	Target defender if within max range.
//  Range is defined as what we can fight in worst case. (stalkerDistanceMax + weaponMaxRange)
//	Return true if target set.
//

bool AttackableCreature::setTargetDefenderWithinMaxRange(void)
{
	bool foundTarget = false;

	ObjectIDList::iterator defenderIt = this->getDefenders()->begin();
	while (defenderIt != this->getDefenders()->end())
	{
		if (CreatureObject* defenderCreature = dynamic_cast<CreatureObject*>(gWorldManager->getObjectById((*defenderIt))))
		{
			if (!defenderCreature->isIncapacitated() && !defenderCreature->isDead())
			{
				if (gWorldManager->objectsInRange(this->getHomePosition(), this->getCellIdForSpawn(), *defenderIt,
												  this->getStalkerDistanceMax() + this->getWeaponMaxRange()))
				{
					// Do only attack objects that have build up enough aggro.
					if (this->attackerHaveAggro(defenderCreature->getId()))
					{
						if (this->getTargetId() != (*defenderIt))
						{
							this->setTarget(*defenderIt);
							// TEST ERU gMessageLib->sendTargetUpdateDeltasCreo6(this);
						}
						foundTarget = true;
						break;
					}
				}
			}
		}
		++defenderIt;
	}
	return foundTarget;
}

//=============================================================================
//
//	Return true if object is within our max range.
//

bool AttackableCreature::isTargetWithinMaxRange(uint64 targetId)
{
	bool foundTarget = false;

	if (CreatureObject* creature = dynamic_cast<CreatureObject*>(gWorldManager->getObjectById(targetId)))
	{
		if (!creature->isIncapacitated() && !creature->isDead())
		{
			if (gWorldManager->objectsInRange(this->getHomePosition(), this->getCellIdForSpawn(), targetId,
											  this->getStalkerDistanceMax() + this->getWeaponMaxRange()))
			{
				foundTarget = true;
			}
		}
	}
	return foundTarget;
}

bool AttackableCreature::isTargetValid(void)
{
	bool foundTarget = false;

	if (this->getTarget())
	{
		// Is this target a valid player?
		PlayerObjectSet* knownPlayers = this->getKnownPlayers();
		PlayerObjectSet::iterator it = knownPlayers->begin();
		while (it != knownPlayers->end())
		{
			if (this->getTarget() == (*it))	// Is the target in the known list of objects?
			{
				if (isTargetWithinMaxRange((*it)->getId()) && (*it)->isConnected()) //this function also checks incap or dead
				{
					foundTarget = true;
				}
				break;
			}
			++it;
		}
	}
	return foundTarget;
}


//=============================================================================
//
//	Get defender out of range.
//	Return untargeted defender Id, or 0.
//

uint64 AttackableCreature::getDefenderOutOfAggroRange(void)
{
	uint64 targetOutOfRange = 0;

	ObjectIDList::iterator defenderIt = this->getDefenders()->begin();
	while (defenderIt != this->getDefenders()->end())
	{
		if (CreatureObject* defenderCreature = dynamic_cast<CreatureObject*>(gWorldManager->getObjectById((*defenderIt))))
		{
			// if (!defenderCreature->isIncapacitated() && !defenderCreature->isDead())
			//if (!defenderCreature->isDead())
			//{
			if (defenderCreature->isIncapacitated() || defenderCreature->isDead() ||
			   (!gWorldManager->objectsInRange(this->getId(), defenderCreature->getId(), this->getMaxAggroRange())))
			{
				targetOutOfRange = defenderCreature->getId();
				break;
			}
			// }
		}
		++defenderIt;
	}
	return targetOutOfRange;
}


bool AttackableCreature::isTargetWithinWeaponRange(void) const
{
	bool inRange = false;
	if (CreatureObject* targetCreature = dynamic_cast<CreatureObject*>(this->getTarget()))
	{
		inRange = gWorldManager->objectsInRange(this->getId(), targetCreature->getId(), this->getWeaponMaxRange());
	}
	/*
	if (inRange)
	{
	}
	else
	{
	}
	*/
	return inRange;
}


void AttackableCreature::equipPrimaryWeapon(void)
{
	if (mPrimaryWeapon)
	{
		this->mEquipManager.removeEquippedObject(CreatureEquipSlot_Hold_Left);

		// if (!this->getEquipManager()->addEquippedObject(mPrimaryWeapon))
		if (this->mEquipManager.addEquippedObject(CreatureEquipSlot_Hold_Left, mPrimaryWeapon))
		{
			mPrimaryWeapon->setParentId(this->getId());
			gMessageLib->sendEquippedListUpdate_InRange(this);
			gMessageLib->sendWeaponIdUpdate(this);

		/*
			gMessageLib->sendEquippedListUpdate_InRange(this);

			PlayerObjectSet* inRangePlayers = this->getKnownPlayers();
			PlayerObjectSet::iterator it = inRangePlayers->begin();

			while(it != inRangePlayers->end())
			{
				PlayerObject* targetObject = (*it);
				gMessageLib->sendBaselinesCREO_6(this,targetObject);
				// gMessageLib->sendCreateTangible(mPrimaryWeapon,targetObject);
				++it;
			}

			// weapon update
			gMessageLib->sendWeaponIdUpdate(this);
			gMessageLib->sendEquippedListUpdate_InRange(this);
			*/
		}
		else
		{
			gLogger->log(LogManager::DEBUG,"AttackableCreature::equipPrimaryWeapon() Can't equip primary weapon");
		}
	}
}


void AttackableCreature::equipSecondaryWeapon(void)
{
	if (mSecondaryWeapon)
	{
		if (this->mEquipManager.addEquippedObject(CreatureEquipSlot_Hold_Left, mSecondaryWeapon))
		{
			gMessageLib->sendEquippedListUpdate_InRange(this);
			gMessageLib->sendWeaponIdUpdate(this);
		}
		else
		{
			gLogger->log(LogManager::DEBUG,"AttackableCreature::equipWeapon() Can't equip secondary weapon\n");
		}
	}
}

void AttackableCreature::unequipWeapon(void)
{
	Weapon* weapon = dynamic_cast<Weapon*>(this->getEquipManager()->getEquippedObject(CreatureEquipSlot_Hold_Left));
	if (weapon)
	{
		this->mEquipManager.removeEquippedObject(CreatureEquipSlot_Hold_Left);

		gMessageLib->sendContainmentMessage_InRange(weapon->getId(), this->getId(), 0xffffffff, this);
		gMessageLib->sendDestroyObject(weapon->getId(), this);
		gMessageLib->sendEquippedListUpdate_InRange(this);

		// The weapon is now owned by the npc inventory. But we have not put it there, yet.
		// In fact, we keep these npc-weapons outside inventory, until we need to loot the them,
		// they are setup as "default weopons" during npc creation.
		/*
		Inventory* inventory = dynamic_cast<Inventory*>(this->getEquipManager()->getEquippedObject(CreatureEquipSlot_Inventory));
		if (inventory)
		{
			weapon->setParentId(inventory->getId());
		}
		*/
	}

	// if weapon slot is empty, equip the unarmed default weapon
	if (!this->mEquipManager.getEquippedObject(CreatureEquipSlot_Hold_Left))
	{
		this->mEquipManager.equipDefaultWeapon();
		gMessageLib->sendEquippedListUpdate_InRange(this);
		gMessageLib->sendWeaponIdUpdate(this);

		// TEST
		// gMessageLib->sendCreateCreature(this,defenderPlayer);
	}
}


void AttackableCreature::handleEvents(void)
{
	// General issues like life and death first.
	if (this->isDead())
	{
		mCombatState = State_Dead;

		if (!gWorldConfig->isTutorial())
		{
			// make a final position update, reading the heightmap, since we may have been "on the move" and y-axis is not correct.
            glm::vec3 newPosition(this->mPosition);

			if (this->getParentId() == 0)
			{
				// Heightmap only works outside.
				newPosition.y = getHeightAt2DPosition(newPosition.x, newPosition.z, true);
				this->updatePosition(this->getParentId(), newPosition);
			}
		}
		return;
	}

	switch (mCombatState)
	{
		case State_Unspawned:
		{
			// Let's get this object into the world.
			if (!this->isSpawned())
			{
				this->spawn();
			}

			if (this->getKnownPlayers()->empty() || !this->isAgressive())
			{
				// We will not start roaming or do any action if there are no one near us watching. Creatures here have a big ego. :)
				mCombatState = State_Idle;
				this->setAiState(NpcIsDormant);
			}
			else
			{
				// mCombatState = State_Idle;
				// this->setAiState(NpcIsReady);

				// We have got some company.
				mCombatState = State_Alerted;
				this->setAiState(NpcIsReady);

				// Setup roaming, if any.
				if (this->isRoaming())
				{
					this->setupRoaming(15, 15);
				}
			}
		}
		break;

		case State_Idle:
		{
			if (!this->getKnownPlayers()->empty())
			{
				// We have got some company.
				mCombatState = State_Alerted;
				this->setAiState(NpcIsReady);

				// Setup roaming, if any.
				if (this->isRoaming())
				{
					// Setup a delay, we do not want all npc to start roam at the same time when a player enters a dormant area.
					// Make the base delay time shorter than normal case, since we can assume we have been dormant.
					// uint64 roamingPeriods = this->getRoamingDelay() / ((uint32)readyDefaultPeriodTime);
					// int64 roamingReadyTicksDelay = (int64)((int64)(roamingPeriods/2) + gRandom->getRand() % (int32) (roamingPeriods));

					int64 roamingReadyTicksDelay = (int64)(this->getRoamingDelay()/2);
					roamingReadyTicksDelay += (int64)(((uint64)gRandom->getRand() * 1000) % (this->getRoamingDelay() + 1));

					this->SetReadyDelay(roamingReadyTicksDelay /(uint32)readyDefaultPeriodTime);

					// this->setupRoaming(15, 15);
				}
			}
		}
		break;

		case State_Alerted:
		{
			if (this->getKnownPlayers()->empty())
			{
				mCombatState = State_Idle;
				this->setAiState(NpcIsDormant);
			}
			else
			{
				// Any attacked us?
				if (this->setTargetDefenderWithinWeaponRange())
				{
					// Yes.
					mCombatState = State_Combat;
					this->setAiState(NpcIsActive);

					// Change pvp-status to agressive.
					PlayerObject* targetPlayer = dynamic_cast<PlayerObject*>(this->getTarget());
					if (targetPlayer)
					{
						gMessageLib->sendUpdatePvpStatus(this,targetPlayer, this->getPvPStatus() | CreaturePvPStatus_Attackable | CreaturePvPStatus_Aggressive | CreaturePvPStatus_Enemy);
					}

					// We may need to chase the target.
					this->setupStalking(activeDefaultPeriodTime);
				}
				// Any new target in range we can aggro??
				else if (this->setTargetInAttackRange())		// Any player within attack range when in aggressiveMode.
				{
					// Yes.
					mCombatState = State_Combat;
					this->setAiState(NpcIsActive);

					// Change pvp-status to agressive.
					PlayerObject* targetPlayer = dynamic_cast<PlayerObject*>(this->getTarget());
					if (targetPlayer)
					{
						gMessageLib->sendUpdatePvpStatus(this,targetPlayer, this->getPvPStatus() | CreaturePvPStatus_Attackable | CreaturePvPStatus_Aggressive | CreaturePvPStatus_Enemy);
					}

					// We may need to chase the target.
					this->setupStalking(activeDefaultPeriodTime);
				}
				// Anyone need our help?
				if (this->needToAssistLair())
				{
					this->executeLairAssist();
				}
				else if (this->needAssist())
				{
					this->executeAssist();
				}
				// Any target we can rush into?
				else if (this->setTargetDefenderWithinMaxRange())	// Defenders within max range (stalkerDistanceMax + weaponMaxRange)
				{
					// Yes.
					mCombatState = State_Combat;
					this->setAiState(NpcIsActive);

					// Change pvp-status to agressive.
					PlayerObject* targetPlayer = dynamic_cast<PlayerObject*>(this->getTarget());
					if (targetPlayer)
					{
						gMessageLib->sendUpdatePvpStatus(this,targetPlayer, this->getPvPStatus() | CreaturePvPStatus_Attackable | CreaturePvPStatus_Aggressive | CreaturePvPStatus_Enemy);
					}

					// We may need to chase the target.
					this->setupStalking(activeDefaultPeriodTime);
				}
				else if (this->getDefenders()->size() != 0)
				{
					mCombatState = State_CombatReady;
					this->setAiState(NpcIsReady);
				}
			}
		}
		break;

		case State_CombatReady:
		{
			if (this->getKnownPlayers()->empty())
			{
				if (this->insideRoamingLimit())
				{
					mCombatState = State_Idle;
					this->setAiState(NpcIsDormant);
				}
				else
				{
					this->enableHoming();
					this->SetReadyDelay(1);	// Want to start the homing asap.
					this->setupRoaming(15, 15);
				}
				// End all combat.
			}
			else if (!this->isTargetValid())
			{
				// We lost our target.
				this->setTarget(NULL);
				// TEST ERU gMessageLib->sendTargetUpdateDeltasCreo6(this);

				mCombatState = State_Alerted;
				this->setAiState(NpcIsReady);

				this->mAssistanceNeededWithId = 0;
				this->mLairNeedAssistanceWithId = 0;
				this->mIsAssistingLair = false;
			}

			if (this->needToAssistLair())
			{
				this->executeLairAssist();
			}
			else if (this->isTargetWithinWeaponRange())
			{
				mCombatState = State_Combat;
				this->setAiState(NpcIsActive);

				// We may need to chase the target.
				this->setupStalking(activeDefaultPeriodTime);
			}
			else if (this->isHoming())
			{
				if (this->needToAssistLair())
				{
					this->executeLairAssist();
				}
				// Continue until I'm home.
				// Anyone need our help?
				else if (this->needAssist())
				{
					this->executeAssist();
				}
			}
			else
			{
				// Any attacked us?
				if (this->setTargetDefenderWithinWeaponRange())
				{
					// Yes.
					mCombatState = State_Combat;
					this->setAiState(NpcIsActive);

					// Change pvp-status to agressive.
					PlayerObject* targetPlayer = dynamic_cast<PlayerObject*>(this->getTarget());
					if (targetPlayer)
					{
						gMessageLib->sendUpdatePvpStatus(this,targetPlayer, this->getPvPStatus() | CreaturePvPStatus_Attackable | CreaturePvPStatus_Aggressive | CreaturePvPStatus_Enemy);
					}

					// We may need to chase the target.
					this->setupStalking(activeDefaultPeriodTime);
				}
				// Any new target in range?
				else if (this->setTargetInAttackRange())
				{
					// Yes.
					mCombatState = State_Combat;
					this->setAiState(NpcIsActive);

					// Change pvp-status to agressive.
					PlayerObject* targetPlayer = dynamic_cast<PlayerObject*>(this->getTarget());
					if (targetPlayer)
					{
						gMessageLib->sendUpdatePvpStatus(this,targetPlayer, this->getPvPStatus() | CreaturePvPStatus_Attackable | CreaturePvPStatus_Aggressive | CreaturePvPStatus_Enemy);
					}

					// We may need to chase the target.
					this->setupStalking(activeDefaultPeriodTime);
				}
				// Any active target we can rush into?
				else if (this->setTargetDefenderWithinMaxRange())	// Defenders within max range (stalkerDistanceMax + weaponMaxRange)
				{
					// Yes.
					mCombatState = State_Combat;
					this->setAiState(NpcIsActive);

					// Change pvp-status to agressive.
					PlayerObject* targetPlayer = dynamic_cast<PlayerObject*>(this->getTarget());
					if (targetPlayer)
					{
						gMessageLib->sendUpdatePvpStatus(this,targetPlayer, this->getPvPStatus() | CreaturePvPStatus_Attackable | CreaturePvPStatus_Aggressive | CreaturePvPStatus_Enemy);
					}

					// We may need to chase the target.
					this->setupStalking(activeDefaultPeriodTime);
				}
				else if (this->getDefenders()->size() == 0)
				{
					mCombatState = State_Alerted;
					this->setAiState(NpcIsReady);
				}
				else if (!this->isHoming())
				{
					if (isTargetValid())
					{
						if (this->targetOutsideRoamingLimit())
						{
							// Only attempt to "walk away" if we are a stalker.
							if (this->isStalker())
							{
								this->enableHoming();
								this->SetReadyDelay(1);	// Want to start the homing asap.
								this->setupRoaming(15, 15);
							}
						}
					}
					else
					{
						// We lost our target.
						this->setTarget(NULL);
						// TEST ERU gMessageLib->sendTargetUpdateDeltasCreo6(this);
					}
				}
			}
		}
		break;

		case State_Combat:
		{
			if (!this->isTargetValid())
			{
				// We lost our target.
				this->setTarget(NULL);
				// TEST ERU gMessageLib->sendTargetUpdateDeltasCreo6(this);

				mCombatState = State_CombatReady;
				this->setAiState(NpcIsReady);

				this->mAssistanceNeededWithId = 0;
				this->mLairNeedAssistanceWithId = 0;
				this->mIsAssistingLair = false;

				this->SetReadyDelay(1);	// Want to start the homing asap, if nothing better to do.
				this->setupRoaming(15, 15);
			}
			else if (this->needToAssistLair())
			{
				this->executeLairAssist();
			}
			else if (!this->checkState((CreatureState)(CreatureState_Combat)))
			{
				// We are not in combat.
				// We may be stalking a target....
				// But.. if someone attack us, we prio to defence ourself.
				// Anyone attacked us?
				if (this->setTargetDefenderWithinWeaponRange())
				{
					// Yes.
					// mCombatState = State_Combat;
					// this->setAiState(NpcIsActive);

					// Change pvp-status to agressive.
					PlayerObject* targetPlayer = dynamic_cast<PlayerObject*>(this->getTarget());
					if (targetPlayer)
					{
						gMessageLib->sendUpdatePvpStatus(this,targetPlayer, this->getPvPStatus() | CreaturePvPStatus_Attackable | CreaturePvPStatus_Aggressive | CreaturePvPStatus_Enemy);
					}

					// We may need to chase the target.
					this->setupStalking(activeDefaultPeriodTime);
				}
				// Any new target in range we can aggro??
				else if (this->setTargetInAttackRange())		// Any player within attack range when in aggressiveMode.
				{
					// Yes.
					// mCombatState = State_Combat;
					// this->setAiState(NpcIsActive);

					// Change pvp-status to agressive.
					PlayerObject* targetPlayer = dynamic_cast<PlayerObject*>(this->getTarget());
					if (targetPlayer)
					{
						gMessageLib->sendUpdatePvpStatus(this,targetPlayer, this->getPvPStatus() | CreaturePvPStatus_Attackable | CreaturePvPStatus_Aggressive | CreaturePvPStatus_Enemy);
					}

					// We may need to chase the target.
					this->setupStalking(activeDefaultPeriodTime);
				}
				else if (this->atStalkLimit())
				{
					// We are at max limit and target is out of range. Let's go home.
					this->enableHoming();
					this->SetReadyDelay(1);	// Want to start the homing asap.
					this->setupRoaming(15, 15);

					// We drop the target, it's out of range.
					this->setTarget(NULL);
					// TEST ERU gMessageLib->sendTargetUpdateDeltasCreo6(this);

					mCombatState = State_CombatReady;
					this->setAiState(NpcIsReady);

					// Clear the current assist target.
					this->mAssistanceNeededWithId = 0;
					this->mLairNeedAssistanceWithId = 0;
					this->mIsAssistingLair = false;

				}
				// else
				//{
				//	// Hunt him down.
				//	this->setupStalking(activeDefaultPeriodTime);
				//}
			}
			else
			{
				// Clear the current assist target.
				this->mAssistanceNeededWithId = 0;
				// this->mLairNeedAssistanceWithId = 0;

				CreatureObject* targetCreature = dynamic_cast<CreatureObject*>(this->getTarget());
				if (targetCreature)
				{
					if (targetCreature->isIncapacitated())
					{
						// Target is incapped, leave him alone, try to find a new target.
						mCombatState = State_CombatReady;
						this->setAiState(NpcIsReady);
					}
					else if (targetCreature->isDead())
					{
						// Target is dead, try to find a new target.
						mCombatState = State_CombatReady;
						this->setAiState(NpcIsReady);
					}
					else if (!this->isTargetWithinWeaponRange() && (!this->isStalker()))
					{
						// Target is out of range, try to find a new target.
						mCombatState = State_CombatReady;
						this->setAiState(NpcIsReady);
					}
					else
					{
						if (this->atStalkLimit())
						{
							// We are at max limit and target is out of range. Let's go home.
							this->enableHoming();
							this->SetReadyDelay(1);	// Want to start the homing asap.
							this->setupRoaming(15, 15);
							mCombatState = State_CombatReady;
							this->setAiState(NpcIsReady);
						}
						else
						{
							// Hunt him down.
							this->setupStalking(activeDefaultPeriodTime);
						}
					}
				}
				else
				{
					// Invalid target.
					mCombatState = State_CombatReady;
					this->setAiState(NpcIsReady);

					assert(false);
				}
			}
		}
		break;

		case State_Halted:
		{
			// Do nothing. Mainly used by tutorial when getting the "bandit" to hold it's fighting animations.
		}
		break;

		default:
		break;
	}
}


uint64 AttackableCreature::handleState(uint64 timeOverdue)
{
	uint64 waitTime = 0;

	switch (mCombatState)
	{
		case State_Dead:
		{
			// Remove us from all timers etc...
			// NpcManager::Instance()->removeNpc(this->getId());
		}
		break;

		case State_Unspawned:
		{
			// This is not a valid state.
			gLogger->log(LogManager::CRITICAL,"AttackableCreature::handleState Invalid state State_Unspawned.");
			// It's a serious isse that we need to investigate.
			assert(false && "AttackableCreature should never be in an unspawned state");

			// Remove us from all timers etc...
			// NpcManager::Instance()->removeNpc(this->getId());
		}
		break;

		case State_Idle:
		{
			// Set me for a long wait...

			// Start roaming timer, then we will have them all running when we get players in range.
			waitTime = this->getRoamingDelay() + (int64)(((uint64)gRandom->getRand() * 1000) % ((this->getRoamingDelay()/2)+1));
		}
		break;

		case State_Alerted:
		{
			// Hm... check this state fairly often.
			waitTime = (uint64)readyDefaultPeriodTime;

			// Any player that we should give a last warning?
			this->showWarningInRange();

			// Are we supposed to do any roaming?
			if (this->isRoaming())
			{
				// Yes, continue roaming
				int32 movementCounter = this->getRoamingSteps();
				if (movementCounter > 0)
				{
					// Incremental moves
					movementCounter--;
					if (movementCounter == 0)
					{
						// Do the final move
						// this->mPosition = this->getDestination();
						this->updatePosition(this->getParentId(), this->getDestination());
					}
					else
					{
						this->moveAndUpdatePosition();
						// waitTime = 1000;
						if (readyDefaultPeriodTime > (int64)timeOverdue)
						{
							waitTime = (readyDefaultPeriodTime - (int64)timeOverdue);
						}
						else
						{
							waitTime = readyDefaultPeriodTime;
						}
					}
					this->setRoamingSteps(movementCounter);
				}
				else
				{
					// We are set to roaming, but ends up here when the roaming sequence is done and we have players nearby.

					// use a delay before we roam next time. We can't sleep because we have players present.
					int64 roamingReadyTicksDelay = this->getReadyDelay();
					if (roamingReadyTicksDelay > 0)
					{
						roamingReadyTicksDelay--;
						if (roamingReadyTicksDelay == 0)
						{
							// Start roaming again.
							setupRoaming(15, 15);
						}
					}
					else
					{
						// Set up a new roaming delay to be used here in this state.
						// uint64 roamingPeriods = this->getRoamingDelay() / ((uint32)readyDefaultPeriodTime);
						// roamingReadyTicksDelay = (int64)((int64)roamingPeriods + gRandom->getRand() % (int32) (roamingPeriods/2));

						roamingReadyTicksDelay = (int64)this->getRoamingDelay();
						roamingReadyTicksDelay += (int64)(((uint64)gRandom->getRand() * 1000) % ((this->getRoamingDelay()/2)+1));
						roamingReadyTicksDelay /= (uint32)readyDefaultPeriodTime;
					}
					this->SetReadyDelay(roamingReadyTicksDelay);
				}
			}
		}
		break;

		case State_CombatReady:
		{
			// Hm... check this state fairly often.
			waitTime = (uint64)readyDefaultPeriodTime;

			// Any player that we should give a last warning?
			this->showWarningInRange();

			// Make peace with players out of range.
			uint64 untargetId;
			if ((untargetId = this->getDefenderOutOfAggroRange()) != 0)
			{
				// Make peace with him.
				this->makePeaceWithDefender(untargetId);
			}

			if (this->isHoming())
			{
				// We are moving home, may still have defenders, but current target was out of range when we hit our max stalking range.
				int32 movementCounter = this->getRoamingSteps();
				if (movementCounter > 0)
				{

					// Incremental moves
					movementCounter--;
					if (movementCounter == 0)
					{
						this->disableHoming();

						// Do the final move
						// this->mPosition = this->getDestination();
						this->updatePosition(this->getParentId(), this->getDestination());
					}
					else
					{
						this->moveAndUpdatePosition();
						if (readyDefaultPeriodTime > (int64)timeOverdue)
						{
							waitTime = (readyDefaultPeriodTime - (int64)timeOverdue);
						}
						else
						{
							waitTime = readyDefaultPeriodTime;
						}
					}
					this->setRoamingSteps(movementCounter);
				}
				else
				{
					this->disableHoming();
				}
			}
		}
		break;

		case State_Combat:
		{
			waitTime = (uint64)activeDefaultPeriodTime;

			int64 activation = this->getCombatTimer();
			if (activation < 0)
			{
				// We are ready to strike. If we are/has been outside range, we just arm the attack.
			}
			else
			{
				activation -= (int64)activeDefaultPeriodTime;		// The average period time.
			}
			if (activation < 0)
			{
				// ask for assistance, somebody my show up and help.
				if (LairObject* lair = dynamic_cast<LairObject*>(gWorldManager->getObjectById(this->getLairId())))
				{
					if (this->getTarget()->getId() != this->mAssistedTargetId)
					{
						if (lair->requestAssistance(this->getTarget()->getId(), this->getId()))
						{
							this->mAssistedTargetId = this->getTarget()->getId();
						}
					}
				}

				if (this->isTargetWithinWeaponRange())
				{
					activation += this->getAttackSpeed();
					NpcManager::Instance()->handleAttack(this, this->getTarget()->getId());
				}
			}
			this->setCombatTimer(activation);

			if (this->isStalker())
			{
				this->stalk();
			}
		}
		break;

		case State_Halted:
		{
			// Do nothing. Mainly used by tutorial when getting the "bandit" to hold it's fighting animations.
		}
		break;


		default:
			gLogger->log(LogManager::NOTICE,"AttackableCreature::handleState Invalid state\n");
			// waitTime = 0;
		break;
	}

	return waitTime;

}


//=============================================================================
//
//	Spawn.
//

void AttackableCreature::spawn(void)
{
	gCreatureSpawnCounter++;

	// Update the world about my presence.

	this->setSpawned();
	if (this->getParentId())
	{
		// insert into cell
		this->setSubZoneId(0);

		if (CellObject* cell = dynamic_cast<CellObject*>(gWorldManager->getObjectById(this->getParentId())))
		{
			cell->addObjectSecure(this);
		}
		else
		{
			gLogger->log(LogManager::CRITICAL,"AttackableCreature::spawnCreature: couldn't find cell %"PRIu64"\n", this->getParentId());
			assert(false && "Unable to locate cell");
			return;
		}
	}
	else
	{
		if (QTRegion* region = gWorldManager->getSI()->getQTRegion(this->mPosition.x, this->mPosition.z))
		{
			this->setSubZoneId((uint32)region->getId());
			region->mTree->addObject(this);
		}
	}
	// Sleeping NPC's should be put in lower prio queue.

	if (this->getKnownPlayers()->empty())
		return;

	// Add us to the world.
	gMessageLib->broadcastContainmentMessage(this,this->getParentId(),4);

	// send out position updates to known players
	this->setInMoveCount(this->getInMoveCount() + 1);

	if (gWorldConfig->isTutorial())
	{
		// We need to get the player object that is the owner of this npc.
		if (this->getPrivateOwner() != 0)
		{
			PlayerObject* playerObject = dynamic_cast<PlayerObject*>(gWorldManager->getObjectById(this->getPrivateOwner()));
			if (playerObject)
			{
				if (this->getParentId())
				{
					// We are inside a cell.
					gMessageLib->sendDataTransformWithParent(this, playerObject);
					gMessageLib->sendUpdateTransformMessageWithParent(this, playerObject);
				}
				else
				{
					gMessageLib->sendDataTransform(this, playerObject);
					gMessageLib->sendUpdateTransformMessage(this, playerObject);
				}
			}
		}
	}
	else
	{
		if (this->getParentId())
		{
			// We are inside a cell.
			gMessageLib->sendDataTransformWithParent053(this);
			gMessageLib->sendUpdateTransformMessageWithParent(this);
		}
		else
		{
			gMessageLib->sendDataTransform053(this);
			gMessageLib->sendUpdateTransformMessage(this);
		}
	}
}

//=============================================================================
//
//	Setup stalking target sequence.
//

void AttackableCreature::setupStalking(uint64 updatePeriodTime)
{
	// Even if we do not stalk, we need to face the enemy.
	// Get target position.
	if (this->getTarget())
	{
		CreatureObject* attacker = dynamic_cast<CreatureObject*>(gWorldManager->getObjectById(this->getTarget()->getId()));
		if (attacker)
		{
			// Update the direction of the npc in the world.
			this->setDirection(attacker->mPosition.x - this->mPosition.x, attacker->mPosition.z - this->mPosition.z);
			if (this->isStalker())
			{
				this->disableHoming();

                float distanceToMove = glm::distance(this->mPosition, attacker->mPosition);
                glm::vec3 destination = attacker->mPosition;

				// Save attackers pos, since we may have to redefine the destination in case of out of allowed distance.
				this->mStalkingTargetDestination = attacker->mPosition;

				// float steps = (distanceToMove * 1000)/(this->getStalkerSpeed() * activeDefaultPeriodTime);
				float steps = (distanceToMove * 1000)/(this->getStalkerSpeed() * updatePeriodTime);

				float xOffset = (destination.x - this->mPosition.x) / steps;
				float yOffset = (destination.y - this->mPosition.y) / steps;
				float zOffset = (destination.z - this->mPosition.z) / steps;

				// Check if we will end up outside given area.
				// We have to be able to move towards a target outside our stalking range.
                if (glm::distance(attacker->mPosition, this->getHomePosition()) >= this->getStalkerDistanceMax())
				{
					// Yes, we have to change the destination.
					// The total distance we may move is from here to the stalker max distance.
                    distanceToMove = this->getStalkerDistanceMax() - (glm::distance(this->mPosition, this->getHomePosition()));
					// steps = distanceToMove/npc->getStalkerSpeed();
					steps = (distanceToMove * 1000)/(this->getStalkerSpeed() * updatePeriodTime);

					destination = this->mPosition;
					destination.x = steps * xOffset;
					destination.y = steps * yOffset;
					destination.z = steps * zOffset;
				}

				this->setDestination(destination);
				this->setStalkerSteps((int32)steps);

				// Save the offset for each movement request.
                this->setPositionOffset(glm::vec3(xOffset, yOffset, zOffset));
			}
		}
	}
}

//=============================================================================
//
//	Setup a new roaming sequence.
//

void AttackableCreature::setupRoaming(int32 maxRangeX, int32 maxRangeZ)
{
	//We can't setup roaming on the tutorial! (NO HEIGHTMAP!)
	if(gWorldConfig->isTutorial())
		return;

	// Get a target position
    glm::vec3 destination;

	// If we already outside roaming area, get back home. We may get here when we stalk other objects.
    if (glm::distance(mPosition, getHomePosition()) >= getRoamingDistanceMax())
	{
		enableHoming();
		SetReadyDelay(1);	// Want to start the homing asap.
		destination = getRandomPosition(getHomePosition(), 2*maxRangeX, 2*maxRangeZ);
	}
	else
	{
		// Verify that we don't roam outside given area.
		destination = getRandomPosition(mPosition, 2*maxRangeX, 2*maxRangeZ);
        while (glm::distance(getHomePosition(), destination) > getRoamingDistanceMax())
		{
			destination = getRandomPosition(mPosition, 2*maxRangeX, 2*maxRangeZ);
		}
	}

	setDestination(destination);

	// Update the direction of the npc in the world.
	setDirection(destination.x - mPosition.x, destination.z - mPosition.z);

	// How many updates do we have to do before npc is at new target position?
    float distanceToMove = glm::distance(mPosition, destination);

	// Dirty hack
	float steps;
	if (isHoming())
	{
		steps = distanceToMove/(getRoamingSpeed() * 3);
	}
	else
	{
		steps = distanceToMove/getRoamingSpeed();
	}

	float xOffset = (destination.x - mPosition.x) / steps;
	float yOffset = (destination.y - mPosition.y) / steps;
	float zOffset = (destination.z - mPosition.z) / steps;

	setRoamingSteps((int32)steps);

	// Calculate and save the offset for each movement request.
    setPositionOffset(glm::vec3(xOffset, yOffset, zOffset));
}


bool AttackableCreature::atStalkLimit() const
{
	bool atLimit = false;

	// Are we supposed to do any stalking?
	if (this->isStalker())
	{
        float distanceToStalkerLimit = this->getStalkerDistanceMax() - (glm::distance(this->mPosition, this->getHomePosition()));
		if (distanceToStalkerLimit <= 2.0)
		{
			atLimit = true;
		}
	}
	return atLimit;
}


bool AttackableCreature::insideRoamingLimit() const
{
	bool atLimit = false;

    float distance = this->getRoamingDistanceMax() - (glm::distance(this->mPosition, this->getHomePosition()));
	if (distance < 0.0)
	{
		atLimit = true;
	}
	return atLimit;
}

bool AttackableCreature::targetOutsideRoamingLimit(void) const
{
	bool atLimit = false;

    float distance = this->getRoamingDistanceMax() - (glm::distance(this->getTarget()->mPosition, this->getHomePosition()));
	if (distance < 0)
	{
		atLimit = true;
	}
	return atLimit;
}


//=============================================================================
//
//	Stalk (follow your target).
//

void AttackableCreature::stalk()
{
	// Are we supposed to do any stalking?
	if (this->isStalker())
	{
		// Yes.
		CreatureObject* attacker = dynamic_cast<CreatureObject*>(gWorldManager->getObjectById(this->getTarget()->getId()));
		if (attacker)
		{
			// TODO: Only if we have changed more than x meters.

			// This will never be true when we are reaching the stalk limit, since the npc->position is not the attackers position.

			// if ((attacker->mPosition == this->getDestination()) == false)
            if (glm::all(glm::equal(attacker->mPosition, this->mStalkingTargetDestination)) == false)
			{
				// Our target may have moved...
				setupStalking(activeDefaultPeriodTime);
			}

			int32 movementCounter = this->getStalkerSteps();
			if (movementCounter > 0)
			{

				// Incremental moves
				movementCounter--;

				if (movementCounter == 0)
				{
					// This our final move.
					// When we attack, we want to stay a little bit from our target. (true until we start using ranged)
                    glm::vec3 positionOffset = this->getPositionOffset();
					positionOffset.x /= 1.50;
					positionOffset.y /= 1.50;
					positionOffset.z /= 1.50;

					// Save the offset for each movement request.
					this->setPositionOffset(positionOffset);
				}
				this->moveAndUpdatePosition();
				this->setStalkerSteps(movementCounter);
			}
		}
	}
}

//=============================================================================
//
//	We have no more defenders.
//	This method should be called as an "event" when we gets compleatly out of combat.
//

void AttackableCreature::inPeace(void)
{
	mWarningTauntSent = false;
	mAttackTauntSent = false;
	this->mAssistedTargetId = 0;
}


bool AttackableCreature::isAttackTauntSent(void) const
{
	return mAttackTauntSent;
}

void AttackableCreature::setAttackTauntSent(void)
{
	mAttackTauntSent = true;
}

bool AttackableCreature::isWarningTauntSent(void) const
{
	return mWarningTauntSent;
}

void AttackableCreature::setWarningTauntSent(void)
{
	mWarningTauntSent = true;
}

void AttackableCreature::clearWarningTauntSent(void)
{
	mWarningTauntSent = false;
}

void AttackableCreature::killEvent(void)
{
	// One creature less in the system.
	gCreatureDeathCounter++;

	// Are we handled by a lair or someone?
	if (this->getLairId())
	{
		// Yes. Report in as dead!
		if (LairObject* lair = dynamic_cast<LairObject*>(gWorldManager->getObjectById(this->getLairId())))
		{
			lair->reportedDead(this->getId());
		}
	}
	else
	{
		if (this->getRespawnDelay() != 0)
		{
			uint64 npcNewId = gWorldManager->getRandomNpNpcIdSequence();
			if (npcNewId != 0)
			{
				// Let's put this sucker into play again.
				this->mTimeToFirstSpawn = ((uint64)gRandom->getRand() * 1000) % (this->getRespawnDelay() + 1);
				NonPersistentNpcFactory::Instance()->requestNpcObject(NpcManager::Instance(),
																		this->getTemplateId(),
																		npcNewId,
																		this->getCellIdForSpawn(),
																		this->getSpawnPosition(),
																		this->getSpawnDirection(),
																		this->getRespawnDelay());
			}
		}
	}
}

void AttackableCreature::respawn(void)
{
	// gCreatureCounter++;

	// The data used below ARE taken from DB, not hard coded as the script version above.

	// The cell we will spawn in.
	this->setParentId(getCellIdForSpawn());

	// Default spawn position.
    glm::vec3 position(getSpawnPosition());

	// Get parent object (lair), if any.
	NPCObject* parent = dynamic_cast<NPCObject*>(gWorldManager->getObjectById(this->getLairId()));
	if (!parent)
	{
		// I have to be my own parent then...
		parent = this;

		// Use the respawn delay set by script, i.e. do nothing, it's already set.
	}
	else
	{
		// Respawn delay. If the creature have an unique delay, use that. Else use the one provided by the parent object.
		if (this->hasInternalAttribute("creature_respawn_delay"))
		{
			uint64 respawnDelay = this->getInternalAttribute<uint64>("creature_respawn_delay");
			// mRespawnDelay = respawnDelay;
			this->setRespawnDelay(respawnDelay);
		}
		else if (parent->hasInternalAttribute("lair_creatures_respawn_delay"))	// Note: parent may be the creature if spawning without a lair.
		{
			uint64 respawnDelay = parent->getInternalAttribute<uint64>("lair_creatures_respawn_delay");
			// mRespawnDelay = respawnDelay;
			this->setRespawnDelay(respawnDelay);
		}
		else
		{
			assert(false && "Missing lair/creature respawn delay value");
			this->setRespawnDelay(1000);
			// mRespawnDelay = 10000;
		}
	}

	// This will give a random spawn delay from 0 up to max delay.
	mTimeToFirstSpawn = (((uint64)gRandom->getRand() * 1000) % (uint32)(this->getRespawnDelay() + 1));

	// Let us get the spawn point. It's 0 - maxSpawnDistance (2D) meters from the lair.
	float maxSpawnDistance = parent->getMaxSpawnDistance();
	if (maxSpawnDistance != 0.0)
	{
		position.x = (position.x - maxSpawnDistance) + (float)(gRandom->getRand() % (int32)(maxSpawnDistance + maxSpawnDistance));
		position.z = (position.z - maxSpawnDistance) + (float)(gRandom->getRand() % (int32)(maxSpawnDistance + maxSpawnDistance));

		// Give them a random dir.
		this->setRandomDirection();
	}
	else
	{
		// Use the supplied direction?
		this->mDirection = getSpawnDirection();
	}
	if (this->getParentId() == 0)
	{
		// Heightmap only works outside.
		position.y = this->getHeightAt2DPosition(position.x, position.z, true);
	}

	this->mHomePosition = position;
	this->mPosition = position;

	mSpawned = false;

	// Already set by factory. mSpawn.mBasic.lairId = this->getLairId();

	if (this->hasInternalAttribute("creature_damage_min"))
	{
		int32 minDamage = this->getInternalAttribute<int32>("creature_damage_min");
		mMinDamage = minDamage;
	}
	else
	{
		assert(false && "Missing creature damage min attribute");
		mMinDamage = 10;
	}

	if (this->hasInternalAttribute("creature_damage_max"))
	{
		int32 maxDamage = this->getInternalAttribute<int32>("creature_damage_max");
		mMaxDamage = maxDamage;
	}
	else
	{
		assert(false && "Missing creature damage max attribute");
		mMaxDamage = 20;
	}

	if (this->hasInternalAttribute("creature_damage_max_range"))
	{
		float maxDamageRange = this->getInternalAttribute<float>("creature_damage_max_range");
		mWeaponMaxRange = maxDamageRange;
	}
	else
	{
		assert(false && "Missing creature damage max range attribute");
		mWeaponMaxRange = 4;
	}

	if (this->hasAttribute("creature_attack"))
	{
		float attackSpeed = this->getAttribute<float>("creature_attack");
		mAttackSpeed = (int64)(attackSpeed * 1000.0);
	}
	else
	{
		assert(false && "Missing creature attack attribute");
		mAttackSpeed = 2000;
	}

	if (this->hasInternalAttribute("creature_xp"))
	{
		uint32 xp = this->getInternalAttribute<uint32>("creature_xp");
		this->setWeaponXp(xp);
	}
	else
	{
		assert(false && "Missing creature xp attribute");
		this->setWeaponXp(0);
	}

	if (this->hasAttribute("aggro"))
	{
		float aggro = this->getAttribute<float>("aggro");
		this->setBaseAggro(aggro);
	}
	else
	{
		assert(false && "Missing agro attribute");
		this->setBaseAggro(40.0);
	}

	if (this->hasInternalAttribute("creature_is_aggressive"))
	{
		bool isAggressive = this->getInternalAttribute<bool>("creature_is_aggressive");
		mIsAgressive = isAggressive;
	}
	else
	{
		assert(false && "Missing creature is aggressive attribute");
		mIsAgressive = false;
	}

	if (this->hasAttribute("stalking"))
	{
		bool isStalker = this->getAttribute<bool>("stalking");
		mIsStalker = isStalker;
	}
	else
	{
		assert(false && "Missing stalking attribute");
		mIsStalker = false;
	}

	if (this->hasInternalAttribute("creature_is_roaming"))
	{
		bool isRoaming = this->getInternalAttribute<bool>("creature_is_roaming");
		mIsRoaming = isRoaming;
	}
	else
	{
		assert(false && "Missing creature is roaming attribute");
		mIsRoaming = false;
	}

	if (this->hasAttribute("killer"))
	{
		bool isKiller = this->getAttribute<bool>("killer");
		mIsKiller = isKiller;
	}
	else
	{
		assert(false && "Missing killer attribute");
		mIsKiller = false;
	}

	if(this->hasAttribute("res_bone"))
	if(this->hasAttribute("res_meat"))
	if(this->hasAttribute("res_hide"))


	if (this->hasInternalAttribute("creature_warning_range"))
	{
		float attackWarningRange = this->getInternalAttribute<float>("creature_warning_range");
		mAttackWarningRange = attackWarningRange;
	}
	else
	{
		assert(false && "Missing creature warning range attribute");
		mAttackWarningRange = 20.0;
	}

	if (mIsAgressive)
	{
		if (this->hasInternalAttribute("creature_attack_range"))
		{
			float attackRange = this->getInternalAttribute<float>("creature_attack_range");
			this->setAttackRange(attackRange);
		}
		else
		{
			assert(false && "Missing creature attack range attribute");
			this->setAttackRange(15.0);
		}
	}

	if (this->hasInternalAttribute("creature_aggro_range"))
	{
		float aggroRange = this->getInternalAttribute<float>("creature_aggro_range");
		mMaxAggroRange = aggroRange;
	}
	else
	{
		assert(false && "Missing creature aggro range attribute");
		mMaxAggroRange = 64;
	}

	if (this->hasInternalAttribute("creature_warning_message"))
	{
		string warningMessage = (int8*)(this->getInternalAttribute<std::string>("creature_warning_message").c_str());
		mAttackWarningMessage = warningMessage;
	}
	else
	{
		// assert(false);
		mAttackWarningMessage = "";
	}

	if (this->hasInternalAttribute("creature_attacking_message"))
	{
		string attackingMessage = (int8*)(this->getInternalAttribute<std::string>("creature_attacking_message").c_str());
		mAttackStartMessage = attackingMessage;
	}
	else
	{
		// assert(false);
		mAttackStartMessage = "";
	}


	if (this->hasInternalAttribute("creature_attacked_message"))
	{
		string attackedMessage = (int8*)(this->getInternalAttribute<std::string>("creature_attacked_message").c_str());
		mAttackedMessage = attackedMessage;
	}
	else
	{
		// assert(false);
		mAttackedMessage = "";
	}

	if (mIsRoaming)
	{
		if (this->hasInternalAttribute("creature_roaming_delay"))
		{
			uint64 roamingDelay = this->getInternalAttribute<uint64>("creature_roaming_delay");
			mRoamingDelay = roamingDelay;
		}
		else
		{
			assert(false && "Missing creature roaming delay attribute");
			mRoamingDelay = 120000;
		}

		if (this->hasInternalAttribute("creature_roaming_speed"))
		{
			float roamingSpeed = this->getInternalAttribute<float>("creature_roaming_speed");
			mRoamingSpeed = roamingSpeed;
		}
		else
		{
			assert(false && "Missing creature roaming speed attribute");
			mRoamingSpeed = 0.5;
		}

		if (this->hasInternalAttribute("creature_roaming_max_distance"))
		{
			float roamingMaxDistance = this->getInternalAttribute<float>("creature_roaming_max_distance");
			mRoamingDistanceMax = roamingMaxDistance;
		}
		else
		{
			assert(false && "Missing creature roaming max distance attribute");
			mRoamingDistanceMax = 64.0;
		}
	}

	if (mIsStalker)
	{
		if (this->hasInternalAttribute("creature_stalking_speed"))
		{
			float stalkingSpeed = this->getInternalAttribute<float>("creature_stalking_speed");
			mStalkerSpeed = stalkingSpeed;
		}
		else
		{
			assert(false && "Missing creature stalking speed attribute");
			mStalkerSpeed = 4.0;
		}

		if (this->hasInternalAttribute("creature_stalking_max_distance"))
		{
			float stalkingMaxDistance = this->getInternalAttribute<float>("creature_stalking_max_distance");
			mStalkerDistanceMax = stalkingMaxDistance;
		}
		else
		{
			assert(false && "Missing creature stalking max distance");
			mStalkerDistanceMax = 64.0;
		}
	}

	if (this->hasInternalAttribute("creature_group_assist"))
	{
		bool groupAssist = this->getInternalAttribute<bool>("creature_group_assist");
		mIsGroupAssist = groupAssist;
	}
	else
	{
		assert(false && "Missing creature group assist attribute");
		mIsGroupAssist = false;
	}

	if (this->hasAttribute("creature_health"))
	{
		int32 health = this->getAttribute<int32>("creature_health");
		this->mHam.mHealth.setCurrentHitPoints(health);
		this->mHam.mHealth.setMaxHitPoints(health);
		this->mHam.mHealth.setBaseHitPoints(health);
	}
	else
	{
		assert(false && "Missing creature health attribute");
		this->mHam.mHealth.setCurrentHitPoints(500);
		this->mHam.mHealth.setMaxHitPoints(500);
		this->mHam.mHealth.setBaseHitPoints(500);
	}

	if (this->hasAttribute("creature_strength"))
	{
		int32 strength = this->getAttribute<int32>("creature_strength");
		this->mHam.mStrength.setCurrentHitPoints(strength);
		this->mHam.mStrength.setMaxHitPoints(strength);
		this->mHam.mStrength.setBaseHitPoints(strength);
	}
	else
	{
		assert(false && "Missing creature strength attribute");
		this->mHam.mStrength.setCurrentHitPoints(500);
		this->mHam.mStrength.setMaxHitPoints(500);
		this->mHam.mStrength.setBaseHitPoints(500);
	}

	if (this->hasAttribute("creature_constitution"))
	{
		int32 constitution = this->getAttribute<int32>("creature_constitution");
		this->mHam.mConstitution.setCurrentHitPoints(constitution);
		this->mHam.mConstitution.setMaxHitPoints(constitution);
		this->mHam.mConstitution.setBaseHitPoints(constitution);
	}
	else
	{
		assert(false && "Missing creature constitution attribute");
		this->mHam.mConstitution.setCurrentHitPoints(500);
		this->mHam.mConstitution.setMaxHitPoints(500);
		this->mHam.mConstitution.setBaseHitPoints(500);
	}



	if (this->hasAttribute("creature_action"))
	{
		int32 action = this->getAttribute<int32>("creature_action");
		this->mHam.mAction.setCurrentHitPoints(action);
		this->mHam.mAction.setMaxHitPoints(action);
		this->mHam.mAction.setBaseHitPoints(action);
	}
	else
	{
		assert(false && "Missing creature action attribute");
		this->mHam.mAction.setCurrentHitPoints(500);
		this->mHam.mAction.setMaxHitPoints(500);
		this->mHam.mAction.setBaseHitPoints(500);
	}

	if (this->hasAttribute("creature_quickness"))
	{
		int32 quickness = this->getAttribute<int32>("creature_quickness");
		this->mHam.mQuickness.setCurrentHitPoints(quickness);
		this->mHam.mQuickness.setMaxHitPoints(quickness);
		this->mHam.mQuickness.setBaseHitPoints(quickness);
	}
	else
	{
		assert(false && "Missing creature quickness attribute");
		this->mHam.mQuickness.setCurrentHitPoints(500);
		this->mHam.mQuickness.setMaxHitPoints(500);
		this->mHam.mQuickness.setBaseHitPoints(500);
	}

	if (this->hasAttribute("creature_stamina"))
	{
		int32 stamina = this->getAttribute<int32>("creature_stamina");
		this->mHam.mStamina.setCurrentHitPoints(stamina);
		this->mHam.mStamina.setMaxHitPoints(stamina);
		this->mHam.mStamina.setBaseHitPoints(stamina);
	}
	else
	{
		assert(false && "Missing creature stamina attribute");
		this->mHam.mStamina.setCurrentHitPoints(500);
		this->mHam.mStamina.setMaxHitPoints(500);
		this->mHam.mStamina.setBaseHitPoints(500);
	}


	if (this->hasAttribute("creature_mind"))
	{
		int32 mind = this->getAttribute<int32>("creature_mind");
		this->mHam.mMind.setCurrentHitPoints(mind);
		this->mHam.mMind.setMaxHitPoints(mind);
		this->mHam.mMind.setBaseHitPoints(mind);
	}
	else
	{
		assert(false && "Missing creature mind attribute");
		this->mHam.mMind.setCurrentHitPoints(500);
		this->mHam.mMind.setMaxHitPoints(500);
		this->mHam.mMind.setBaseHitPoints(500);
	}

	if (this->hasAttribute("creature_focus"))
	{
		int32 focus = this->getAttribute<int32>("creature_focus");
		this->mHam.mFocus.setCurrentHitPoints(focus);
		this->mHam.mFocus.setMaxHitPoints(focus);
		this->mHam.mFocus.setBaseHitPoints(focus);
	}
	else
	{
		assert(false && "Missing creature focus attribute");
		this->mHam.mFocus.setCurrentHitPoints(500);
		this->mHam.mFocus.setMaxHitPoints(500);
		this->mHam.mFocus.setBaseHitPoints(500);
	}

	if (this->hasAttribute("creature_willpower"))
	{
		int32 willpower = this->getAttribute<int32>("creature_willpower");
		this->mHam.mWillpower.setCurrentHitPoints(willpower);
		this->mHam.mWillpower.setMaxHitPoints(willpower);
		this->mHam.mWillpower.setBaseHitPoints(willpower);
	}
	else
	{
		assert(false && "Missing creature willpower attribute");
		this->mHam.mWillpower.setCurrentHitPoints(500);
		this->mHam.mWillpower.setMaxHitPoints(500);
		this->mHam.mWillpower.setBaseHitPoints(500);
	}
	this->mHam.calcAllModifiedHitPoints();


	// Let WorldManager own the object.
	// gWorldManager->addObject(this, true);

	// All init is done, just the spawn in the world is left.
	// Put this sucker in the Dormant queue.
	this->clearSpawned();

	gWorldManager->addDormantNpc(this->getId(), mTimeToFirstSpawn);
}

//=============================================================================
//
//	Assist some from my "group"
//

void AttackableCreature::assistCreature(uint64 targetId)
{
	// if (this->isGroupAssist())
	{
		if (this->mAssistanceNeededWithId == 0)
		{
			this->mAssistanceNeededWithId = targetId;
		}
	}
}

//=============================================================================
//
//	Assist my lair.
//

void AttackableCreature::assistLair(uint64 targetId)
{
	// if (this->mLairNeedAssistanceWithId == 0)
	{
		this->mLairNeedAssistanceWithId = targetId;
		this->mIsAssistingLair = false;
	}
}

//=============================================================================
//
//	Anyone need assistance?
//

bool AttackableCreature::needAssist(void)
{
	bool assistNeeded = false;
	if (this->mAssistanceNeededWithId != 0)
	{
		// Will not assist if target is outside stalking limit.
		assistNeeded = isTargetWithinMaxRange(this->mAssistanceNeededWithId);

		if (!assistNeeded)
		{
			// Clear the current assist target.
			this->mAssistanceNeededWithId = 0;
		}
	}
	return assistNeeded;
}

//=============================================================================
//
//	Lair need assistance?
//

bool AttackableCreature::needToAssistLair(void)
{
	bool status = ((this->mLairNeedAssistanceWithId != 0) && !this->mIsAssistingLair);
	if (status)
	{
		this->mIsAssistingLair = true;
	}
	return status;
}

void AttackableCreature::executeAssist(void)
{
	// Are we interested to help?

	// Yes. Go and get that sucker.
	Object* object = gWorldManager->getObjectById(this->mAssistanceNeededWithId );
	if (object)
	{
		this->setTarget(object->getId());
		// TEST ERU gMessageLib->sendTargetUpdateDeltasCreo6(this);

		mCombatState = State_Combat;
		this->setAiState(NpcIsActive);

		// We may need to chase the target.
		this->setupStalking(activeDefaultPeriodTime);
	}
}

void AttackableCreature::executeLairAssist(void)
{
	// Are we interested to help?

	// Yes. Go and get that sucker.
	Object* object = gWorldManager->getObjectById(this->mLairNeedAssistanceWithId );
	if (object)
	{
		this->setTarget(object->getId());
		// TEST ERU gMessageLib->sendTargetUpdateDeltasCreo6(this);

		mCombatState = State_Combat;
		this->setAiState(NpcIsActive);

		// We may need to chase the target.
		this->setupStalking(activeDefaultPeriodTime);
	}
}

float AttackableCreature::getMaxSpawnDistance(void)
{
	float maxSpawnDistance = 0.0;	// Will default to fix spawn position.

	// Max spawn distance for creatures.
	if (this->hasInternalAttribute("creature_max_spawn_distance"))
	{
		maxSpawnDistance = this->getInternalAttribute<float>("creature_max_spawn_distance");
	}

	return maxSpawnDistance;
}
