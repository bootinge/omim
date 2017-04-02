package com.mapswithme.maps.ads;

import android.os.Parcel;
import android.os.Parcelable;
import android.support.annotation.NonNull;

public final class Banner implements Parcelable
{
  private static final int TYPE_NONE = 0;
  private static final int TYPE_FACEBOOK = 1;
  private static final int TYPE_RB = 2;

  public static final Creator<Banner> CREATOR = new Creator<Banner>()
  {
    @Override
    public Banner createFromParcel(Parcel in)
    {
      return new Banner(in);
    }

    @Override
    public Banner[] newArray(int size)
    {
      return new Banner[size];
    }
  };

  @NonNull
  private final String mId;
  private final int mType;

  public Banner(@NonNull String id, int type)
  {
    mId = id;
    mType = type;
  }

  protected Banner(Parcel in)
  {
    mId = in.readString();
    mType = in.readInt();
  }

  @NonNull
  public String getId()
  {
    return mId;
  }

  @NonNull
  String getProvider()
  {
    switch (mType)
    {
      case TYPE_FACEBOOK:
        return Providers.FACEBOOK;
      case TYPE_RB:
        return Providers.MY_TARGET;
      default:
        throw new AssertionError("Unsupported banner type: " + mType);
    }
  }

  @Override
  public String toString()
  {
    return "Banner{" +
           "mId='" + mId + '\'' +
           ", provider=" + getProvider() +
           '}';
  }

  @Override
  public int describeContents()
  {
    return 0;
  }

  @Override
  public void writeToParcel(Parcel dest, int flags)
  {
    dest.writeString(mId);
    dest.writeInt(mType);
  }

  @Override
  public boolean equals(Object o)
  {
    if (this == o) return true;
    if (o == null || getClass() != o.getClass()) return false;

    Banner banner = (Banner) o;

    return  mId.equals(banner.mId);
  }

  @Override
  public int hashCode()
  {
    return mId.hashCode();
  }
}