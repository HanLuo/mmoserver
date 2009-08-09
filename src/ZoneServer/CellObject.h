/*
---------------------------------------------------------------------------------------
This source file is part of swgANH (Star Wars Galaxies - A New Hope - Server Emulator)
For more information, see http://www.swganh.org


Copyright (c) 2006 - 2008 The swgANH Team

---------------------------------------------------------------------------------------
*/

#ifndef ANH_ZONESERVER_CELL_OBJECT_H
#define ANH_ZONESERVER_CELL_OBJECT_H

#include "staticobject.h"
#include "LogManager/LogManager.h"

//=============================================================================

/*
- cells, need a building as parent
*/

class CellObject :	public StaticObject
{
	friend class CellFactory;

	public:

		CellObject();
		CellObject(uint64 id,uint64 parentId,const string model = "object/cell/shared_cell.iff");
		~CellObject();

		ObjectList*	getChilds(){ return &mChildObjects; }
		void		addChild(Object* object){ mChildObjects.push_back(object); }
		bool		removeChild(Object* object);
		bool		checkForChild(Object* object);

		uint32		getLoadCount(){ return mTotalLoadCount; }
		void		setLoadCount(uint32 count){ mTotalLoadCount = count; }

	private:

		ObjectList	mChildObjects;
		uint32		mTotalLoadCount;
	
};

//=============================================================================

#endif